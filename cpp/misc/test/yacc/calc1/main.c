#include "calc.h"
#include <stdio.h>

int main(int argc, char* argv[]) {
    const char* input = "1+2=";
    CalcParseResult* result = CalcParse(input);

    printf("%s%d\n", input, result->ret);

    DestroyCalcParseResult(result);
    return 0;
}
