#ifndef THIN_DELTA_SCANNER_H
#define THIN_DELTA_SCANNER_H

enum token {
	TK_DIFF = 258,
	TK_LEFT,
	TK_UUID,
	TK_TIME,
	TK_SAME,
	TK_RIGHT,
	TK_BEGIN,
	TK_STRING,
	TK_LENGTH,
	TK_DIFFERENT,
	TK_LEFT_ONLY,
	TK_RIGHT_ONLY,
	TK_SUPERBLOCK,
	TK_TRANSACTION,
	TK_NR_DATA_BLOCKS,
	TK_DATA_BLOCK_SIZE,
};

extern FILE* yyin;
extern int yylex(void);

extern char *str_value;

#endif
