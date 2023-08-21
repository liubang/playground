%option noyywrap nodefault yylineno
%{
/*
 |------------------------------------------------------------------
 | linger test
 |------------------------------------------------------------------
 | @author    : liubang
 | @date      : 16/10/27 下午8:28
 | @copyright : (c) iliubang.cn
 | @license   : MIT (http://opensource.org/licenses/MIT)
 |------------------------------------------------------------------
 */

#include <stdio.h>
# include "calc.h"
#include "calc_bison.h"

%}

%%

"+"  return ADD;
"-"  return SUB;
"*"  return MUL;
"/"  return DIV;
"\n" return CR;

([1-9][0-9]*)|0|([0-9]+\.[0-9]+) {
	double d;
	sscanf(yytext, "%lf", &d);
	yylval.double_value = d;
	return DOUBLE_LITERAL;
}

[ \t] ;
. {
	fprintf(stderr, "lexical error.\n");
	exit(1);
}
%%
