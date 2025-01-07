#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>

#include <linux/fs.h> /* ioctl BLKDISCARD */

#include "thin_delta_scanner.h"

#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE     0x02 /* de-allocates range */
#endif

struct snap_info {
	char *vg_name;
	char *lv_name;
	char *thin_pool_name;
	char *dm_path;
	int thin_id;
	bool active;
};

struct chunk {
	uint64_t magic;
	uint64_t offset;
	uint64_t length;
	uint32_t cmd;
} __attribute__((packed));

enum cmd {
	/* defines the binary format */
	CMD_DATA = 0,
	CMD_UNMAP = 1,
	CMD_BEGIN_STREAM = 2,
	CMD_END_STREAM = 3,

	/* Forward compat for optional chunks */
	CMD_FLAG_OPTIONAL_INFO = 1U << 31,
};

static const char *PGM_NAME = "thin-send-recv";
static const char *const LOCKFILE_PATH = "/var/run/thin-send-recv.lock";
static const uint64_t MAGIC_VALUE_1_1 = 0x24C4F02AAE2E4FA9ULL;
static const uint64_t MAGIC_VALUE_1_0 = 0xCA7F00D5DE7EC7EDULL;
static const uint64_t OLD_MAGIC = 0xE85BC5636CC72A05ULL;
static const uint32_t CATCH_SIGNALS = 1 << SIGABRT | 1 << SIGALRM |
	1 << SIGBUS | 1 << SIGFPE | 1 << SIGHUP | 1 << SIGINT |
	1 << SIGPIPE | 1 << SIGPWR | 1 << SIGQUIT | 1 << SIGSEGV |
	1 << SIGTERM | 1 << SIGUSR1 | 1 << SIGUSR2 | 1 << SIGXCPU |
	1 << SIGXFSZ;

static uint64_t expect_magic;

/* statistics for some plausibility checks */
struct stream_stats {
	uint64_t n_chunks;
	uint64_t n_data;
	uint64_t n_unmap;
} __attribute__((packed));

struct stream_context {
	int in_fd;
	int out_fd;

	uint64_t n_chunks;
	uint64_t n_data;
	uint64_t n_unmap;
	int n_begin_stream;
	int n_end_stream;
};

static void parse_diff(struct stream_context *ctx);
static void parse_dump(struct stream_context *ctx);
static void send_end_stream(struct stream_context *ctx);
static void usage_exit(const struct option *long_options, const char *reason);
static void get_snap_info(const char *snap_name, struct snap_info *info);
static int checked_asprintf(char **strp, const char *fmt, ...);
static int system_fmt(const char *fmt, ...);
static void send_header(int out_fd, loff_t begin, size_t length, enum cmd cmd);
static void send_chunk(int in_fd, int out_fd, loff_t begin, size_t length, size_t block_size);
static void thin_send_vol(const char *vol_name, int out_fd);
static void thin_send_diff(const char *snap1_name, const char *snap2_name, int out_fd);
static void thin_receive(const char *snap_name, int in_fd);
static bool process_input(struct stream_context *ctx);
static int lockfile_lock(void);
static void lockfile_unlock(int lockfile_fd);
static int reserve_metadata_snap(const char *thin_pool_dm_path);
static void release_metadata_snap(const char *thin_pool_dm_path);

static const char *data_for_signal_handler;

static bool unsupported_unmap_is_fatal = false;

enum stream_format {
	STREAM_FORMAT_AUTO,
	STREAM_FORMAT_1_0,
	STREAM_FORMAT_1_1,
};

enum {	OPT_STREAM_FORMAT = 0x1000 };

static enum stream_format stream_format = STREAM_FORMAT_AUTO;
static enum stream_format to_stream_format(const char *opt)
{
	if (!strcmp(opt, "auto")) return STREAM_FORMAT_AUTO;
	if (!strcmp(opt, "1.0")) return STREAM_FORMAT_1_0;
	if (!strcmp(opt, "1.1")) return STREAM_FORMAT_1_1;

	fprintf(stderr, "unknown stream format specifier \"%s\"; should be one of \"auto\", \"1.0\", \"1.1\".\n", opt);
	exit(10);
}

int main(int argc, char **argv)
{
	if (argv == NULL || argc < 1) {
		fputs("thin_send_recv: Nonexistent or empty arguments array, aborting.\n", stderr);
		abort();
	}

	/* Set the program name to whatever was used to call the program */
	PGM_NAME = argv[0];

	static struct option long_options[] = {
		{"version",   no_argument, 0, 'v' },
		{"send",      no_argument, 0, 's' },
		{"receive",   no_argument, 0, 'r' },
		{"allow-tty", no_argument, 0, 't' },
		{"about",     no_argument, 0, 'a' },
		{"accept-stream-format",     required_argument, 0, OPT_STREAM_FORMAT },
		{0,         0,             0, 0 }
	};

	bool send_mode = false, receive_mode = false, allow_tty = false;
	int option_index, c;

	do {
		c = getopt_long(argc, argv, "vsrta", long_options, &option_index);
		switch (c) {
		case 'a':
			puts("Philipp Reisner <philipp.reisner@linbit.com> wrote this because\n"
			     "it is embarrassing that this does not ship with the LVM tools.\n"
			     "\n"
			     "License: GPLv3");
			exit(0);
		case 'v':
			puts(VERSION);
			exit(0);
		case 's':
			send_mode = true;
			break;
		case 'r':
			receive_mode = true;
			break;
		case 't':
			allow_tty = true;
			break;
		case OPT_STREAM_FORMAT:
			stream_format = to_stream_format(optarg);
			break;
		case -1:
			break;
			/* case '?': unknown opt*/
		default:
			usage_exit(long_options, "unknown option\n");
		}
	} while (c != -1);

	if (!(send_mode || receive_mode)) {
		if (strstr(argv[0], "send"))
			send_mode = true;
		if (strstr(argv[0], "receive") || strstr(argv[0], "recv"))
			receive_mode = true;
	}
	if (!(send_mode || receive_mode) || (send_mode && receive_mode))
		usage_exit(long_options, "Use --send or --receive\n");

	if (send_mode) {
		if (optind != argc - 1 && optind != argc -2)
			usage_exit(long_options, "One or two positional arguments expected\n");

		if (!allow_tty && isatty(fileno(stdout))) {
			fprintf(stderr, "Not dumping the data stream onto your terminal\n"
				"If you really like that try --allow-tty\n");
			exit(10);
		}

		/* TODO: add some meta data? */
		send_header(fileno(stdout), 0, 0, CMD_BEGIN_STREAM);
		/* CMD_END_STREAM sent as last action in thin_send_vol/thin_send_diff */

		if (optind == argc - 1)
			thin_send_vol(argv[optind], fileno(stdout));
		else if (optind == argc - 2)
			thin_send_diff(argv[optind], argv[optind + 1], fileno(stdout));
	} else {
		if (optind != argc - 1)
			usage_exit(long_options, "One positional argument expected\n");

		if (!allow_tty && isatty(fileno(stdin))) {
			fprintf(stderr, "Expecting a data stream on stdin\n"
				"If you really like that try --allow-tty\n");
			exit(10);
		}

		thin_receive(argv[optind], fileno(stdin));
	}

	return 0;
}

static char *get_thin_pool_dm_path(const struct snap_info *snap)
{
	char *thin_pool_dm_path, *cmdline;
	int matches;
	FILE *f;

	checked_asprintf(&cmdline, "lvs --noheadings -o lv_dm_path %s/%s", snap->vg_name, snap->thin_pool_name);
	f = popen(cmdline, "r");
	if (!f) {
		perror("popen failed");
		exit(10);
	}

	matches = fscanf(f, " %ms", &thin_pool_dm_path);
	if (matches != 1) {
		fprintf(stderr, "failed to parse lvs output %d cmdline=%s\n", matches, cmdline);
		exit(10);
	}
	pclose(f);

	return thin_pool_dm_path;
}

static void thin_send_diff(const char *snap1_name, const char *snap2_name, int out_fd)
{
	struct stream_context ctx = { 0, };
	struct snap_info snap1, snap2;
	char tmp_file_name[] = "/tmp/thin_send_recv_XXXXXX";
	char *thin_pool_dm_path;
	int err, tmp_fd, snap2_fd;

	get_snap_info(snap1_name, &snap1);
	get_snap_info(snap2_name, &snap2);

	tmp_fd = mkstemp(tmp_file_name);
	if (tmp_fd == -1) {
		perror("failed creating tmp file");
		exit(10);
	}
	fcntl(tmp_fd, F_SETFD, FD_CLOEXEC);

	thin_pool_dm_path = get_thin_pool_dm_path(&snap2);
	const int lockfile_fd = lockfile_lock();
	if (lockfile_fd == -1)
		exit(10);
	err = reserve_metadata_snap(thin_pool_dm_path);
	if (err) {
		unlink(tmp_file_name);
		lockfile_unlock(lockfile_fd);
		exit(10);
	}

	err = system_fmt("thin_delta -m --snap1 %d --snap2 %d %s_tmeta > %s",
			 snap1.thin_id, snap2.thin_id, thin_pool_dm_path,
			 tmp_file_name);
	unlink(tmp_file_name);

	release_metadata_snap(thin_pool_dm_path);
	lockfile_unlock(lockfile_fd);

	if (err)
		exit(10);

	yyin = fdopen(tmp_fd, "r");
	if (!yyin) {
		perror("failed to open tmpfile");
		exit(10);
	}

	if (!snap2.active)
		system_fmt("lvchange --ignoreactivationskip --activate y %s", snap2_name);

	snap2_fd = open(snap2.dm_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (snap2_fd == -1) {
		fprintf(stderr, "failed to open %s with %d %s\n", snap2.dm_path, errno, strerror(errno));
		exit(10);
	}

	ctx.in_fd = snap2_fd;
	ctx.out_fd = out_fd;
	ctx.n_chunks = 2; /* begin and end marker count */
	parse_diff(&ctx);
	send_end_stream(&ctx);

	fclose(yyin);
	close(snap2_fd);

	if (!snap2.active)
		system_fmt("lvchange --activate n %s", snap2_name);
}

static void thin_send_vol(const char *vol_name, int out_fd)
{
	struct stream_context ctx = { 0, };
	struct snap_info vol;
	char tmp_file_name[] = "/tmp/thin_send_recv_XXXXXX";
	char *thin_pool_dm_path;
	int err, tmp_fd, vol_fd;

	get_snap_info(vol_name, &vol);

	tmp_fd = mkstemp(tmp_file_name);
	if (tmp_fd == -1) {
		perror("failed creating tmp file");
		exit(10);
	}
	fcntl(tmp_fd, F_SETFD, FD_CLOEXEC);

	thin_pool_dm_path = get_thin_pool_dm_path(&vol);
	const int lockfile_fd = lockfile_lock();
	if (lockfile_fd == -1)
		exit(10);
	err = reserve_metadata_snap(thin_pool_dm_path);
	if (err) {
		unlink(tmp_file_name);
		lockfile_unlock(lockfile_fd);
		exit(10);
	}

	err = system_fmt("thin_dump -m --dev-id %d %s_tmeta > %s",
			 vol.thin_id, thin_pool_dm_path, tmp_file_name);
	unlink(tmp_file_name);

	release_metadata_snap(thin_pool_dm_path);
	lockfile_unlock(lockfile_fd);
	if (err)
		exit(10);

	yyin = fdopen(tmp_fd, "r");
	if (!yyin) {
		perror("failed to open tmpfile");
		exit(10);
	}

	vol_fd = open(vol.dm_path, O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (vol_fd == -1) {
		perror("failed to open snap2");
		exit(10);
	}

	ctx.in_fd = vol_fd;
	ctx.out_fd = out_fd;
	ctx.n_chunks = 2; /* begin and end marker count */
	parse_dump(&ctx);
	send_end_stream(&ctx);

	fclose(yyin);
	close(vol_fd);
}

static void thin_receive(const char *snap_name, int in_fd)
{
	struct snap_info snap;
	char *snap_file_name;
	int out_fd;
	bool cont;
	struct stream_context ctx = { 0, };

	get_snap_info(snap_name, &snap);

	checked_asprintf(&snap_file_name, "/dev/%s/%s", snap.vg_name, snap.lv_name);
	out_fd = open(snap_file_name, O_WRONLY | O_CLOEXEC);
	if (out_fd == -1) {
		perror("failed to open snap");
		exit(10);
	}
	free(snap_file_name);

	ctx.in_fd = in_fd;
	ctx.out_fd = out_fd;
	do {
		cont = process_input(&ctx);
	} while (cont);

	if (ctx.n_begin_stream && !ctx.n_end_stream) {
		fprintf(stderr, "Missing END_STREAM marker.\n");
		exit(10);
	}
	if (ctx.n_chunks == 0) {
		fprintf(stderr, "Empty input.\n");
		if (stream_format == STREAM_FORMAT_1_1)
			exit(10);
	}

	close(out_fd);
}

static void get_snap_info(const char *snap_name, struct snap_info *info)
{
	char *cmdline;
	char *attr;
	int matches;
	FILE *f;

	checked_asprintf(&cmdline, "lvs --noheadings -o vg_name,lv_name,pool_lv,lv_dm_path,thin_id,attr %s", snap_name);
	f = popen(cmdline, "r");
	if (!f) {
		perror("popen failed");
		exit(10);
	}

	matches = fscanf(f, " %ms %ms %ms %ms %d %ms",
			 &info->vg_name,
			 &info->lv_name,
			 &info->thin_pool_name,
			 &info->dm_path,
			 &info->thin_id,
			 &attr);
	if (matches != 6 || strlen(attr) < 5) {
		fprintf(stderr, "failed to parse lvs output %d cmdline=%s\n", matches, cmdline);
		exit(10);
	}
	info->active = attr[4] == 'a';

	free(cmdline);
	free(attr);
	pclose(f);
}

static void usage_exit(const struct option *long_options, const char *reason)
{
	const struct option *opt;

	if (reason)
		fputs(reason, stderr);

	fputs("\nUSAGE:\n"
	      "thin_send [options] snapshot1 snapshot2\n"
	      "thin_send [options] volume|snapshot\n"
	      "thin_recv [options] volume|snapshot\n"
	      "\n"
	      "Options:\n", stderr);

	for (opt = long_options; opt->name; opt++)
		fprintf(stderr, "  --%s | -%c\n", opt->name, opt->val);

	exit(10);
}

static void expected_got(int expected, int got)
{
	fprintf(stderr, "Got unexpected token %d. Expected a %d\n", got, expected);
	exit(20);
}

static int expect(int expected)
{
        int token;
        token = yylex();
        if (token != expected) {
                expected_got(expected, token);
        }
        return token;
}

static void expect_tag(int tag)
{
	expect('<');
	expect(tag);
}

static void expect_end_tag(int tag)
{
	expect('<');
	expect('/');
	expect(tag);
	expect('>');
}

static const char *expect_attribute(int attribute)
{
	expect(attribute);
	expect('=');
	expect(TK_VALUE);
	return str_value;
}

/**
 * Parse the "flags" attribute and the following "version" attribute, ignoring flags if it does
 * not exist.
 */
static void expect_flags_and_or_version()
{
	int token = yylex();
	switch(token) {
		case TK_FLAGS:
			expect('=');
			expect(TK_VALUE);
			// After flags, we expect version, so parse this here as well.
			expect(TK_VERSION);
			/* fall through */
		case TK_VERSION:
			expect('=');
			expect(TK_VALUE);
			break;
		default:
			fprintf(stderr, "Got unexpected token %d. Expected a %d or %d\n", token, TK_FLAGS, TK_VERSION);
			exit(20);
	}

}

static void parse_diff(struct stream_context *ctx)
{
	long block_size;
	int in_fd = ctx->in_fd;
	int out_fd = ctx->out_fd;

	expect_tag(TK_SUPERBLOCK);
	expect_attribute(TK_UUID);
	expect_attribute(TK_TIME);
	expect_attribute(TK_TRANSACTION);
	block_size = atol(expect_attribute(TK_DATA_BLOCK_SIZE));
	expect_attribute(TK_NR_DATA_BLOCKS);
	expect('>');

	expect_tag(TK_DIFF);
	expect_attribute(TK_LEFT);
	expect_attribute(TK_RIGHT);
	expect('>');

	while (true) {
		loff_t begin;
		size_t length;
		int token;

		expect('<');
		token = yylex();
		switch (token) {
		case TK_DIFFERENT:
		case TK_SAME:
		case TK_RIGHT_ONLY:
		case TK_LEFT_ONLY:
			begin = atoll(expect_attribute(TK_BEGIN));
			length = atoll(expect_attribute(TK_LENGTH));
			expect('/');
			expect('>');

			break;
		case '/':
			goto break_loop;
		}
		if (token == TK_DIFFERENT || token == TK_RIGHT_ONLY) {
			send_chunk(in_fd, out_fd,
				   begin * block_size * 512,
				   length * block_size * 512,
				   block_size * 512);
			ctx->n_data++;
			ctx->n_chunks++;
		} else if (token == TK_LEFT_ONLY) {
			send_header(out_fd,
				    begin * block_size * 512,
				    length * block_size * 512,
				    CMD_UNMAP);
			ctx->n_unmap++;
			ctx->n_chunks++;
		}
	}
break_loop:
	expect(TK_DIFF);
	expect('>');

	expect_end_tag(TK_SUPERBLOCK);
}

static void parse_dump(struct stream_context *ctx)
{
	long block_size;
	int in_fd = ctx->in_fd;
	int out_fd = ctx->out_fd;

	expect_tag(TK_SUPERBLOCK);
	expect_attribute(TK_UUID);
	expect_attribute(TK_TIME);
	expect_attribute(TK_TRANSACTION);
	expect_flags_and_or_version();
	block_size = atol(expect_attribute(TK_DATA_BLOCK_SIZE));
	expect_attribute(TK_NR_DATA_BLOCKS);
	expect('>');

	expect_tag(TK_DEVICE);
	expect_attribute(TK_DEV_ID);
	expect_attribute(TK_MAPPED_BLOCKS);
	expect_attribute(TK_TRANSACTION);
	expect_attribute(TK_CREATION_TIME);
	expect_attribute(TK_SNAP_TIME);
	expect('>');

	while (true) {
		loff_t begin;
		size_t length;
		int token;

		expect('<');
		token = yylex();
		switch (token) {
		case TK_SINGLE_MAPPING:
			length = 1;
			begin = atoll(expect_attribute(TK_ORIGIN_BLOCK));
			expect_attribute(TK_DATA_BLOCK);
			expect_attribute(TK_TIME);
			break;
		case TK_RANGE_MAPPING:
			begin = atoll(expect_attribute(TK_ORIGIN_BEGIN));
			expect_attribute(TK_DATA_BEGIN);
			length = atoll(expect_attribute(TK_LENGTH));
			expect_attribute(TK_TIME);
			break;

		case '/':
			goto break_loop;
		}
		expect('/');
		expect('>');

		send_chunk(in_fd, out_fd,
			   begin * block_size * 512,
			   length * block_size * 512,
			   block_size * 512);
		ctx->n_chunks++;
		ctx->n_data++;
	}
break_loop:
	expect(TK_DEVICE);
	expect('>');

	expect_end_tag(TK_SUPERBLOCK);
}

static void write_all(int out_fd, const char *data, const size_t count)
{
	size_t write_offset = 0;
	do {
		const ssize_t write_rc = write(out_fd, &data[write_offset], count - write_offset);
		if (write_rc > 0) {
			write_offset += write_rc;
		} else if (!(write_rc == -1 && errno == EINTR)) {
			perror("write failed");
			exit(10);
		}
	} while (write_offset < count);
}

static void send_end_stream(struct stream_context *ctx)
{
	/* Maybe add "total bytes in stream", "checksum over full stream"? */
	struct stream_stats stats = {
		.n_chunks = htobe64(ctx->n_chunks),
		.n_data = htobe64(ctx->n_data),
		.n_unmap = htobe64(ctx->n_unmap)
	};
	send_header(ctx->out_fd, 0, sizeof(stats), CMD_END_STREAM);
	write_all(ctx->out_fd, (const char *)&stats, sizeof(stats));
}

static void send_header(int out_fd, loff_t begin, size_t length, enum cmd cmd)
{
	struct chunk chunk = {
		.magic = htobe64(MAGIC_VALUE_1_1),
		.offset = htobe64(begin),
		.length = htobe64(length),
		.cmd = htobe32(cmd),
	};
	write_all(out_fd, (const char *) &chunk, sizeof(chunk));
}

static bool is_fifo(int fd)
{
	struct stat sb;
	int err;

	err = fstat(fd, &sb);
	if (err) {
		perror("fstat failed");
		exit(10);
	}
	return S_ISFIFO(sb.st_mode);
}

static size_t splice_data(int in_fd, int out_fd, size_t len)
{
	ssize_t ret;

	do {
		ret = splice(in_fd, NULL, out_fd, NULL, len, SPLICE_F_MOVE);
		if (ret == 0) {
			break;
		} else if (ret == -1) {
			perror("splice(data)");
			exit(10);
		}
		len -= ret;
		posix_fadvise(out_fd, 0, 0, POSIX_FADV_DONTNEED);
	} while (len);

	return len;
}

static size_t splice_data_with_fifo(int in_fd, int out_fd, size_t len, int pipe_fd[2])
{
	ssize_t ret_pipe;
	ssize_t ret_out;

	do {
		ret_pipe = splice(in_fd, NULL, pipe_fd[1], NULL, len, SPLICE_F_MOVE);
		if (ret_pipe == 0) {
			break;
		} else if (ret_pipe == -1) {
			perror("splice(data_with_fifo)");
			exit(10);
		}

		ret_out = splice_data(pipe_fd[0], out_fd, ret_pipe);
		if (ret_out != 0) {
			fprintf(stderr, "Incomplete splice out: %zd bytes remaining.\n", ret_out);
			break;
		}
		len -= ret_pipe;
	} while (len);

	return len;
}

static void copy_data(int in_fd, loff_t *in_off,
		     int out_fd, loff_t *out_off,
		     size_t len)
{
	static int one_is_fifo = -1;
	static int pipe_fd[2];

	if (one_is_fifo == -1) {
		one_is_fifo = is_fifo(in_fd) || is_fifo(out_fd);
		if (!one_is_fifo) {
			int ret = pipe2(pipe_fd, O_CLOEXEC);
			if (ret) {
				perror("pipe()");
				exit(10);
			}
		}
	}

	if (in_off) {
		off_t r = lseek(in_fd, *in_off, SEEK_SET);
		if (r == -1) {
			fprintf(stderr, "lseek(in_fd, %jd, SEEK_SET): %m\n", (intmax_t)*in_off);
			exit(10);
		}
	}

	if (out_off) {
		off_t r = lseek(out_fd, *out_off, SEEK_SET);
		if (r == -1) {
			fprintf(stderr, "lseek(out_fd, %jd, SEEK_SET): %m\n", (intmax_t)*out_off);
			exit(10);
		}
	}

	if (one_is_fifo)
		len = splice_data(in_fd, out_fd, len);
	else
		len = splice_data_with_fifo(in_fd, out_fd, len, pipe_fd);
	if (len != 0) {
		fprintf(stderr, "Incomplete copy_data, %zu bytes missing.\n", len);
		exit(10);
	}
}

static void send_chunk(int in_fd, int out_fd, loff_t begin, size_t length, size_t block_size)
{
	send_header(out_fd, begin, length, CMD_DATA);
	copy_data(in_fd, &begin, out_fd, NULL, length);
}

size_t read_complete(struct stream_context *ctx, void *const buf, const size_t requested_count)
{
	const int fd = ctx->in_fd;
	char *const read_buf = buf;
	size_t completed_count = 0;

	while (completed_count < (ssize_t) requested_count) {
		void *const read_ptr = &read_buf[completed_count];
		const ssize_t read_bytes = read(fd, read_ptr, requested_count - completed_count);
		if (read_bytes > 0) {
			completed_count += read_bytes;
		} else if (read_bytes < 0) {
			/* No-op if EINTR, otherwise read error */
			if (errno == EINTR)
				continue; /* interrupted read, retry */
			perror("read()");
			exit(10);
		} else {
			/* read_bytes == 0, end of file */
			/* clean end of file, nothing was read, completed_count == 0 */
			if (completed_count == 0)
				break;
			/* else partially read buffer, error */
			if (ctx->n_end_stream)
				fprintf(stderr, "Trailing garbage beyond END_STREAM marker.\n");
			else
				fprintf(stderr, "Truncated input, bytes expected: %zu, got: %zu.\n",
					requested_count, completed_count);
			exit(10);
		}
	}

	return completed_count;
}

static void cmd_unmap(int out_fd, off_t byte_offset, size_t byte_length)
{
	/* For huge devices, and if device-mapper passes this down to the backend,
	 * this may be large, and take some time.
	 * Do it in "chunks" per call so this can show progress
	 * and can be interrupted, if necessary. */
	const size_t bytes_per_iteration = 1024*1024*1024;

	size_t bytes_left = byte_length;
	off_t offset = byte_offset;
	size_t chunk;
	uint64_t range[2];
	int ret;

	/* TODO
	 * maybe properly align start and end of ioctl ranges,
	 * and explicitly pwrite zero-out unaligned leading/trailing partial "extents".
	 * In case thin allocation chunk size does not match between sender and receiver.
	 */

	while (bytes_left > 0) {
		chunk = bytes_per_iteration < bytes_left ? bytes_per_iteration : bytes_left;
		range[0] = offset;
		range[1] = chunk; /* len */

		ret = ioctl(out_fd, BLKDISCARD, &range);

		if (ret == -1) {
			bool ignore =
				errno == EOPNOTSUPP &&
				unsupported_unmap_is_fatal == false;

			fprintf(stderr, "unmap(,%zd,%zu) failed: %s%s\n",
				byte_offset, byte_length, strerror(errno),
				ignore ? "" : " -- ignored\n");
			if (!ignore)
				exit(10);
		}
		offset += chunk;
		bytes_left -= chunk;
	}
}

static void verify_end_stream(struct stream_context *ctx, uint64_t offset, uint64_t length)
{
	/* offset does not carry meaning (yet), expected to be 0.
	 * length is expected to be sizeof(stream_stats).
	 * "Forward compat" could allow larger length to add additional info,
	 * but I think those should rather be added in "optional chunks" just
	 * before the END_STREAM marker.
	 * Treat them as additional magic numbers. */

	struct stream_stats stats;
	size_t count;
	if (!(offset == 0 && length == sizeof(stats))) {
		fprintf(stderr, "Cannot verify END_STREAM marker chunk: o: %"PRIu64", l: %"PRIu64"\n",
			offset, length);
		exit(10);
	}
	errno = 0;
	count = read_complete(ctx, &stats, sizeof(stats));
	if (count != sizeof(stats)) {
		fprintf(stderr, "Cannot verify END_STREAM marker, incomplete stats: expected %zu bytes, got %zu\n",
			sizeof(stats), count);
		exit(10);
	}
	stats.n_chunks = be64toh(stats.n_chunks);
	stats.n_unmap = be64toh(stats.n_unmap);
	stats.n_data = be64toh(stats.n_data);
	if (ctx->n_chunks != stats.n_chunks
	||  ctx->n_unmap != stats.n_unmap
	||  ctx->n_data != stats.n_data) {
		fprintf(stderr,
			"END_STREAM marker mismatch: chunks/unmap/data: stream: %"PRIu64"/%"PRIu64"/%"PRIu64", marker: %"PRIu64"/%"PRIu64"/%"PRIu64"\n",
			ctx->n_chunks, ctx->n_unmap, ctx->n_data,
			stats.n_chunks, stats.n_unmap, stats.n_data
			);
		exit(10);
	}
}

static bool process_input(struct stream_context *ctx)
{
	int in_fd = ctx->in_fd;
	int out_fd = ctx->out_fd;
	struct chunk chunk;
	uint64_t recv_magic_value;
	off_t offset;
	size_t length;
	enum cmd cmd;
	int ret;

	ret = read_complete(ctx, &chunk, sizeof(chunk));
	if (ret == 0)
		return false;
	assert(ret == sizeof(chunk));

	ctx->n_chunks++;
	if (ctx->n_end_stream) {
		fprintf(stderr, "Stream continued beyond END_STREAM marker\n");
		exit(10);
	}

	recv_magic_value = be64toh(chunk.magic);
	offset = be64toh(chunk.offset);
	length = be64toh(chunk.length);
	cmd = be32toh(chunk.cmd);

	if (ctx->n_chunks == 1) {
		if (recv_magic_value == MAGIC_VALUE_1_1) {
			if (stream_format == STREAM_FORMAT_1_1
			||  stream_format == STREAM_FORMAT_AUTO) {
				expect_magic = MAGIC_VALUE_1_1;
			} else {
				fprintf(stderr, "Found current version magic, but was told to only accept older version magic.\n");
			}
		} else if (recv_magic_value == OLD_MAGIC) {
			fputs("Received data contains the magic value of a previous version of this program.\n"
			      "Make sure that the version of this program that is used on the sending side matches\n"
			      "the version of this program used on the receiving side.\n",
			      stderr);
		} else if (recv_magic_value == MAGIC_VALUE_1_0) {
			if (stream_format == STREAM_FORMAT_1_0
			||  stream_format == STREAM_FORMAT_AUTO) {
				/* silently accept previous format stream */
				expect_magic = MAGIC_VALUE_1_0;
			} else {
				fprintf(stderr, "Found old version magic, but was told to only accept current version magic.\n");
			}
		} else {
			fprintf(stderr, "Magic value mismatch, encountered unknown value 0x%llX\n",
				(unsigned long long) recv_magic_value);
		}

		if (!expect_magic)
			exit(10);
	}

	if (expect_magic != recv_magic_value) {
		fprintf(stderr, "Magic value mismatch, expected 0x%llX, found 0x%llX\n",
			(unsigned long long) expect_magic,
			(unsigned long long) recv_magic_value);
		exit(10);
	}

	if (ctx->n_chunks == 1 && cmd != CMD_BEGIN_STREAM && expect_magic != MAGIC_VALUE_1_0) {
		fprintf(stderr, "Stream does not start with BEGIN_STREAM\n");
		exit(10);
	}

	if (expect_magic == MAGIC_VALUE_1_0 && cmd != CMD_DATA && cmd != CMD_UNMAP) {
		fprintf(stderr, "Old format stream containing unknown cmd chunk: %u\n", cmd);
		exit(10);
		/*
		 * FIXME: rather silently accept, for bug to bug compat?
		 * Previous version would ignore unknown cmd values.
		 */
	}

	switch (cmd) {
	case CMD_DATA:
		copy_data(in_fd, NULL, out_fd, &offset, length);
		ctx->n_data++;
		break;

	case CMD_UNMAP:
		/* we'd like to "punch hole".
		 * But the VFS layer will not allow us to use FALLOC_FL_NO_HIDE_STALE.
		 * And without that, this translates to blockdev_issue_zeroout,
		 * but the block layer rejects "efficient zeroout", because
		 * device mapper thin does not implement it.
		ret = fallocate(out_fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE|FALLOC_FL_NO_HIDE_STALE, offset, length);
		if (ret == -1) {
			perror("fallocate(, FALLOC_FL_PUNCH_HOLE,) failed");
			exit(10);
		}
		 */
		cmd_unmap(out_fd, offset, length);
		ctx->n_unmap++;
		break;

	/* below is not even reached for MAGIC_VALUE_1_0 */
	case CMD_BEGIN_STREAM:
		/* TODO store something useful in it, do something useful with it? */
		if (ctx->n_chunks != 1) {
			fprintf(stderr, "BEGIN_STREAM must occur only once, at the start of the stream\n");
			exit(10);
		}
		ctx->n_begin_stream++;
		break;
	case CMD_END_STREAM:
		/* TODO store something useful in it, do something useful with it? */
		if (ctx->n_begin_stream != 1) {
			fprintf(stderr, "END_STREAM without BEGIN_STREAM!?\n");
			exit(10);
		}
		verify_end_stream(ctx, offset, length);
		ctx->n_end_stream++;
		break;
	default:
		if (cmd & CMD_FLAG_OPTIONAL_INFO) {
			size_t remaining = length;

			fprintf(stderr, "Unrecognized optional chunk 0x%x, length %zu\n", cmd, length);
			while (remaining > 0) {
				char sink[512];
				size_t skip = sizeof(sink);

				if (skip > remaining)
					skip = remaining;
				if (read_complete(ctx, sink, skip) != skip) {
					fputs("Truncated input.\n", stderr);
					exit(10);
				}
				remaining -= skip;
			}
		} else {
			fprintf(stderr, "Unrecognized chunk 0x%x, length %zu\n", cmd, length);
			exit(10);
		}
	}

	return true;
}

static int checked_asprintf(char **strp, const char *fmt, ...)
{
        va_list ap;
        int chars;

        va_start(ap, fmt);
        chars = vasprintf(strp, fmt, ap);
        va_end(ap);

        if (chars == -1) {
                fprintf(stderr, "vasprintf() failed. Out of memory?\n");
                exit(10);
        }

        return chars;
}

static int system_fmt(const char *fmt, ...)
{
	va_list ap;
	char *cmdline;
        int chars;
	int ret;

        va_start(ap, fmt);
        chars = vasprintf(&cmdline, fmt, ap);
        va_end(ap);

        if (chars == -1) {
                fprintf(stderr, "vasprintf() failed. Out of memory?\n");
                exit(10);
        }

	ret = system(cmdline);
	if (!(WIFEXITED(ret) && WEXITSTATUS(ret) == 0)) {
		fprintf(stderr, "cmd %s exited with %d\n", cmdline, WEXITSTATUS(ret));
		return WEXITSTATUS(ret);
	}
	free(cmdline);

	return 0;
}

static int lockfile_lock(void)
{
	int lockfile_fd = open(LOCKFILE_PATH, O_CREAT | O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (lockfile_fd != -1) {
		const int lock_rc = flock(lockfile_fd, LOCK_EX);
		if (lock_rc != 0) {
			const char* const error_msg = strerror(errno);
			fprintf(stderr, "%s: Cannot obtain a lock on lock file %s\n",
			        PGM_NAME, LOCKFILE_PATH);
			fprintf(stderr, "    Error: %s\n", error_msg);
			close(lockfile_fd);
			lockfile_fd = -1;
		}
	} else {
		const char* const error_msg = strerror(errno);
		fprintf(stderr, "%s: Cannot open lock file %s\n",
		        PGM_NAME, LOCKFILE_PATH);
		fprintf(stderr, "    Error: %s\n", error_msg);
	}
	return lockfile_fd;
}

static void lockfile_unlock(const int lockfile_fd)
{
	if (lockfile_fd != -1) {
		flock(lockfile_fd, LOCK_UN);
		close(lockfile_fd);
	}
}

static void release_metadata_upon_signal(int signal)
{
	char *tpool;

	fprintf(stderr, "%s: Terminated by signal %s %d, relasing metadata-snap\n",
		PGM_NAME, strsignal(signal), signal);

	asprintf(&tpool, "%s-tpool", data_for_signal_handler);
	execlp("dmsetup", "dmsetup", "message", tpool, "0", "release_metadata_snap", NULL);
	/* if execlp returned there was an error, errno should be set here. */
	fprintf(stderr, "%s: execlp() returned %d %s\n", PGM_NAME, errno, strerror(errno));
	_exit(10);
}

static void set_signals(void (*handler)(int))
{
	struct sigaction action	= {
		.sa_handler = handler,
	};
	int signum, err, f;
	int signums_to_set = CATCH_SIGNALS;

	sigemptyset(&action.sa_mask);

	while ((f = ffs(signums_to_set))) {
		signum = f - 1;
		signums_to_set &= ~(1U << signum);
		err = sigaction(signum, &action, NULL);
		if (err)
			fprintf(stderr, "sigaction(%s) returned %d %s\n",
				strsignal(signum), errno, strerror(errno));
	}
}

static int reserve_metadata_snap(const char *thin_pool_dm_path)
{
	int err;

	data_for_signal_handler = thin_pool_dm_path;
	set_signals(&release_metadata_upon_signal);
	err = system_fmt("dmsetup message %s-tpool 0 reserve_metadata_snap",
			 thin_pool_dm_path);
	if (err)
		fprintf(stderr, "LVM metadata_snap is reserved. You can free it by running:\n\n"
			"dmsetup message %s-tpool 0 release_metadata_snap\n\n"
			"Only do that if nothing else is using it.\n",
			thin_pool_dm_path);

	return err;
}

static void release_metadata_snap(const char *thin_pool_dm_path)
{
	system_fmt("dmsetup message %s-tpool 0 release_metadata_snap",
		   thin_pool_dm_path);
	set_signals(SIG_DFL);
	data_for_signal_handler = NULL;
}
