#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include "thin_delta_scanner.h"

static void parse();

int main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"--version", no_argument, 0, 0 },
		{0,           0,           0, 0 }
	};

	int option_index;
	int c;

	do {
		c = getopt_long(argc, argv, "v", long_options, &option_index);
		switch (c) {
		case 'v':
			fprintf(stdout, "%s [--version] snapshot1 snapshot2\n", argv[0]);
			break;
		case '?': /* unknown opt*/
		default: ;
		}
	} while (c != -1);

	if (optind != argc - 1 /* 2 */) {
		fprintf(stderr, "two positional arguments expected\n");
		exit(10);
	}

	yyin = fopen(argv[optind], "r");
	if (!yyin) {
		fprintf(stderr, "failed to open file\n");
		exit(10);
	}

	parse();
	fclose(yyin);
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
	expect(TK_STRING);
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
