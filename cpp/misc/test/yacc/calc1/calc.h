#pragma once

#include <stdio.h>

void yyerror(char* s, ...);

extern int yylineno;
extern FILE* yyin;
extern int yylex(void);
