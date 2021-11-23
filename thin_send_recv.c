#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
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
	uint32_t length;
	uint32_t cmd;
} __attribute__((packed));

enum cmd {
	CMD_DATA,
	CMD_UNMAP,
};

static const char *PGM_NAME = "thin-send-recv";
static const char *const LOCKFILE_PATH = "/var/run/thin-send-recv.lock";
static const uint64_t MAGIC_VALUE = 0xe85bc5636cc72a05;
static const uint32_t CATCH_SIGNALS = 1 << SIGABRT | 1 << SIGALRM |
	1 << SIGBUS | 1 << SIGFPE | 1 << SIGHUP | 1 << SIGINT |
	1 << SIGPIPE | 1 << SIGPWR | 1 << SIGQUIT | 1 << SIGSEGV |
	1 << SIGTERM | 1 << SIGUSR1 | 1 << SIGUSR2 | 1 << SIGXCPU |
	1 << SIGXFSZ;

static void parse_diff(int in_fd, int out_fd);
static void parse_dump(int in_fd, int out_fd);
static void usage_exit(const struct option *long_options, const char *reason);
static void get_snap_info(const char *snap_name, struct snap_info *info);
static int checked_asprintf(char **strp, const char *fmt, ...);
static int system_fmt(const char *fmt, ...);
static void send_header(int out_fd, loff_t begin, size_t length, enum cmd cmd);
static void send_chunk(int in_fd, int out_fd, loff_t begin, size_t length, size_t block_size);
static void thin_send_vol(const char *vol_name, int out_fd);
static void thin_send_diff(const char *snap1_name, const char *snap2_name, int out_fd);
static void thin_receive(const char *snap_name, int in_fd);
static bool process_input(int in_fd, int out_fd);
static int lockfile_lock(void);
static void lockfile_unlock(int lockfile_fd);
static int reserve_metadata_snap(const char *thin_pool_dm_path);
static void release_metadata_snap(const char *thin_pool_dm_path);

static const char *data_for_signal_handler;

int main(int argc, char **argv)
{
	if (argc >= 1)
		/* Set the program name to whatever was used to call the program */
		PGM_NAME = argv[0];
	static struct option long_options[] = {
		{"version",   no_argument, 0, 'v' },
		{"send",      no_argument, 0, 's' },
		{"receive",   no_argument, 0, 'r' },
		{"allow-tty", no_argument, 0, 't' },
		{"about",     no_argument, 0, 'a' },
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

	parse_diff(snap2_fd, out_fd);

	fclose(yyin);
	close(snap2_fd);

	if (!snap2.active)
		system_fmt("lvchange --activate n %s", snap2_name);
}

static void thin_send_vol(const char *vol_name, int out_fd)
{
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

	parse_dump(vol_fd, out_fd);

	fclose(yyin);
	close(vol_fd);
}

static void thin_receive(const char *snap_name, int in_fd)
{
	struct snap_info snap;
	char *snap_file_name;
	int out_fd;
	bool cont;

	get_snap_info(snap_name, &snap);

	checked_asprintf(&snap_file_name, "/dev/%s/%s", snap.vg_name, snap.lv_name);
	out_fd = open(snap_file_name, O_WRONLY | O_CLOEXEC);
	if (out_fd == -1) {
		perror("failed to open snap");
		exit(10);
	}
	free(snap_file_name);

	do {
		cont = process_input(in_fd, out_fd);
	} while (cont);

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
	if (matches != 6 && strlen(attr) < 5) {
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

static void parse_diff(int in_fd, int out_fd)
{
	long block_size;

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
		} else if (token == TK_LEFT_ONLY) {
			send_header(out_fd,
				    begin * block_size * 512,
				    length * block_size * 512,
				    CMD_UNMAP);
		}

	}
break_loop:
	expect(TK_DIFF);
	expect('>');

	expect_end_tag(TK_SUPERBLOCK);
}

static void parse_dump(int in_fd, int out_fd)
{
	long block_size;

	expect_tag(TK_SUPERBLOCK);
	expect_attribute(TK_UUID);
	expect_attribute(TK_TIME);
	expect_attribute(TK_TRANSACTION);
	expect_attribute(TK_FLAGS);
	expect_attribute(TK_VERSION);
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
	}
break_loop:
	expect(TK_DEVICE);
	expect('>');

	expect_end_tag(TK_SUPERBLOCK);
}

static void send_header(int out_fd, loff_t begin, size_t length, enum cmd cmd)
{
	int ret;
	struct chunk chunk = {
		.magic = htobe64(MAGIC_VALUE),
		.offset = htobe64(begin),
		.length = htobe32(length),
		.cmd = htobe32(cmd),
	};

	ret = write(out_fd, &chunk, sizeof(chunk));
	if (ret == -1) {
		perror("write failed");
		exit(10);
	} else if (ret != sizeof(chunk)) {
		fprintf(stderr, "write returned %d instead of %ld\n", ret, sizeof(chunk));
		exit(10);
	}
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

static int splice_data(int in_fd, int out_fd, size_t len)
{
	int ret;

	do {
		ret = splice(in_fd, NULL, out_fd, NULL, len, SPLICE_F_MOVE);
		if (ret == 0) {
			return 0;
		} else if (ret == -1) {
			perror("splice(data)");
			exit(10);
		}
		len -= ret;
		posix_fadvise(out_fd, 0, 0, POSIX_FADV_DONTNEED);
	} while (len);

	return 0;
}

static int splice_data_with_fifo(int in_fd, int out_fd, size_t len, int pipe_fd[2])
{
	int ret;

	do {
		ret = splice(in_fd, NULL, pipe_fd[1], NULL, len, SPLICE_F_MOVE);
		if (ret == 0) {
			return 0;
		} else if (ret == -1) {
			perror("splice(data_with_fifo)");
			exit(10);
		}

		splice_data(pipe_fd[0], out_fd, ret);
		len -= ret;
	} while (len);

	return 0;
}

static int copy_data(int in_fd, loff_t *in_off,
		     int out_fd, loff_t *out_off,
		     size_t len, size_t buffer_size)
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
		if (r == -1)
			return -1;
	}

	if (out_off) {
		off_t r = lseek(out_fd, *out_off, SEEK_SET);
		if (r == -1)
			return -1;
	}

	if (one_is_fifo)
		return splice_data(in_fd, out_fd, len);
	else
		return splice_data_with_fifo(in_fd, out_fd, len, pipe_fd);
}

static void send_chunk(int in_fd, int out_fd, loff_t begin, size_t length, size_t block_size)
{
	int ret;

	send_header(out_fd, begin, length, CMD_DATA);

	ret = copy_data(in_fd, &begin, out_fd, NULL, length, block_size);
	if (ret == -1) {
		perror("copy_data() failed");
		exit(10);
	}
}

ssize_t read_complete(int fd, void *buf, size_t count)
{
	ssize_t ret, sum = 0;

	do {
		ret = read(fd, buf, count);
		if (ret == -1)
			return ret;
		else if (ret == 0)
			return sum;
		count -= ret;
		buf += ret;
		sum += ret;
	} while (count);

	return sum;
}

static bool process_input(int in_fd, int out_fd)
{
	struct chunk chunk;
	off_t offset;
	size_t length;
	enum cmd cmd;
	int ret;

	ret = read_complete(in_fd, &chunk, sizeof(chunk));
	if (ret == -1) {
		perror("read failed");
		exit(10);
	} else if (ret == 0) {
		return false;
	}

	if (htobe64(MAGIC_VALUE) != chunk.magic) {
		fprintf(stderr, "Magic value missmatch!\n");
		exit(10);
	}

	offset = be64toh(chunk.offset);
	length = be32toh(chunk.length);
	cmd = be32toh(chunk.cmd);

	switch (cmd) {
	case CMD_DATA:
		ret = copy_data(in_fd, NULL, out_fd, &offset, length, 65536);
		if (ret == -1) {
			perror("copy_data() failed");
			exit(10);
		}
		break;

	case CMD_UNMAP:
		ret = fallocate(out_fd, FALLOC_FL_PUNCH_HOLE, offset, length);
		if (ret == -1) {
			perror("fallocate(, FALLOC_FL_PUNCH_HOLE,) failed");
			exit(10);
		}
		break;
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
	int lockfile_fd = open(LOCKFILE_PATH, O_CREAT | O_RDONLY);
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
