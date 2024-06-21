#include "calc.h"
#include "calc_flex.h"
#include <stdarg.h>

extern int Calcparse(yyscan_t, CalcParseResult* result);

CalcParseResult* CalcParse(const char* sql) {
    int ret = 0;
    FILE* fin = fmemopen((void*)sql, strlen(sql), "r");
    CalcParseResult* result = (CalcParseResult*)malloc(sizeof(CalcParseResult));

    yyscan_t scanner;
    Calclex_init(&scanner);
    Calcset_in(fin, scanner);
    ret = Calcparse(scanner, result);
    Calclex_destroy(scanner);
    fclose(fin);
    if (ret) {
        printf("oops!!!\n");
        result->is_error = 1;
    }
    return result;
}

void DestroyCalcParseResult(CalcParseResult* result) {
    if (result) {
        free(result);
    }
}
