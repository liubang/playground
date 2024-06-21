#pragma once

#include <stdio.h>

typedef struct CalcParseResult {
    int is_error;
    int ret;
} CalcParseResult;

extern CalcParseResult* CalcParse(const char* sql);
extern void DestroyCalcParseResult(CalcParseResult* result);
