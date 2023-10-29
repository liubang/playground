#include "calc.h"
#include <stdio.h>

int main(int argc, char* argv[]) {
    const char* input = "1+2\n";
    CalcResult* result = CalcParse(input);
    printf("%s=%f\n", input, Eval(result->ast));
    DestroyCalcResult(result);
    return 0;
}
