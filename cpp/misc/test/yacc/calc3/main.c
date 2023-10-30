//=====================================================================
//
// main.c -
//
// Created by liubang on 2023/10/29 23:47
// Last Modified: 2023/10/29 23:47
//
//=====================================================================
#include "calc.h"
#include <stdio.h>

int main(int argc, char* argv[]) {
    const char* input = "1+2*3+(5-3)\n";
    CalcResult* result = CalcParse(input);
    printf("%s=%f\n", input, Eval(result->ast));
    DestroyCalcResult(result);
    return 0;
}
