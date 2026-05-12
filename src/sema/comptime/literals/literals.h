#ifndef CONST_EVAL_BIN_LITERALS
#define CONST_EVAL_BIN_LITERALS


#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

bool parse_float_literal(const char *text, double *out);
bool parse_int_literal(const char *text, int64_t *out);

#endif