%{
#include <stdio.h>
#include "flux.h"
#include "flux_parser.h"
%}

%option nounput noyywrap nodefault yylineno reentrant
%option prefix="Flux"


%%

"+" |
"-" |
"*" |
"/" |
"=" |
"|" |
"," |
";" |
"(" |
")"     { return yytext[0]; }


%%
