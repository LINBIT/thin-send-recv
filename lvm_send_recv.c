#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include "thin_delta_scanner.h"

struct snap_info {
	char *vg_name;
	char *lv_name;
	char *thin_pool_name;
	int thin_id;
};

static void parse();
static void usage_exit(const char *prog_name, const char *reason);
static void get_snap_info(const char *snap_name, struct snap_info *info);
static int checked_asprintf(char **strp, const char *fmt, ...);
static void checked_system(const char *fmt, ...);

int main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"--version", no_argument, 0, 0 },
		{0,           0,           0, 0 }
	};

	struct snap_info snap1, snap2;
	char tmp_file_name[] = "lvm_send_recv_XXXXXXXX";
	int option_index, c, tmp_fd;

	do {
		c = getopt_long(argc, argv, "v", long_options, &option_index);
		switch (c) {
		case 'v':
			puts("0.11\n");
			exit(0);
		case '?': /* unknown opt*/
		default: ;
		}
	} while (c != -1);

	if (optind != argc - 2) {
		usage_exit(argv[0], "two positional arguments expected\n");
		exit(10);
	}

	get_snap_info(argv[optind], &snap1);
	get_snap_info(argv[optind + 1], &snap2);

	tmp_fd = mkstemp(tmp_file_name);
	if (tmp_fd == -1) {
		perror("failed creating tmp file");
		exit(10);
	}

	checked_system("dmsetup message /dev/mapper/%s--%s-tpool 0 reserve_metadata_snap",
		       snap1.vg_name, snap1.thin_pool_name);

	checked_system("thin_delta  -m --snap1 %d --snap2 %d /dev/mapper/%s--%s_tmeta > %s",
		       snap1.thin_id, snap2.thin_id, snap2.vg_name, snap2.thin_pool_name, tmp_file_name);
	unlink(tmp_file_name);

	checked_system("dmsetup message /dev/mapper/%s--%s-tpool 0 release_metadata_snap",
		       snap1.vg_name, snap1.thin_pool_name);

	yyin = fdopen(tmp_fd, "r");
	if (!yyin) {
		perror("failed to open file");
		exit(10);
	}

	yyin = fopen(argv[optind], tmp_file_name);

	parse();
	fclose(yyin);
}

static void get_snap_info(const char *snap_name, struct snap_info *info)
{
	char *cmdline;
	int matches;
	FILE *f;

	checked_asprintf(&cmdline, "lvs --noheadings -o vg_name,lv_name,pool_lv,thin_id %s", snap_name);
	f = popen(cmdline, "r");
	if (!f) {
		perror("popen failed");
		exit(10);
	}

	matches = fscanf(f, " %ms %ms %ms %d",
			 &info->vg_name,
			 &info->lv_name,
			 &info->thin_pool_name,
			 &info->thin_id);
	if (matches != 4) {
		fprintf(stderr, "failed to parse lvs output cmdline=%s\n", cmdline);
		exit(10);
	}
	fclose(f);
}

static void usage_exit(const char *prog_name, const char *reason)
{
	if (reason)
		fputs(reason, stderr);

	fprintf(stderr, "%s [--version] snapshot1 snapshot2\n", prog_name);
	exit(10);
}


/* */
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

static void parse()
{
	long data_block_size;

	expect_tag(TK_SUPERBLOCK);
	expect_attribute(TK_UUID);
	expect_attribute(TK_TIME);
	expect_attribute(TK_TRANSACTION);
	data_block_size = atol(expect_attribute(TK_DATA_BLOCK_SIZE));
	expect_attribute(TK_NR_DATA_BLOCKS);
	expect('>');

	expect_tag(TK_DIFF);
	expect_attribute(TK_LEFT);
	expect_attribute(TK_RIGHT);
	expect('>');

	while (true) {
		long long begin, length;
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
			printf("%lld %lld\n", begin, length);
		}
	}
break_loop:
	expect(TK_DIFF);
	expect('>');

	expect_end_tag(TK_SUPERBLOCK);
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

static void checked_system(const char *fmt, ...)
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
		exit(10);
	}
}
