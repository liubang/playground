%{
/*
 |------------------------------------------------------------------
 | linger test
 |------------------------------------------------------------------
 | @author    : liubang
 | @date      : 16/10/27 下午8:43
 | @copyright : (c) iliubang.cn
 | @license   : MIT (http://opensource.org/licenses/MIT)
 |------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>

#define YY_DECL
#include "calc_flex.h"
#include "calc.h"
#include "calc_bison.h"

extern int Calclex(YYSTYPE* yylval, yyscan_t yyscanner, CalcParseResult* result);
extern int Calcerror(yyscan_t scanner, CalcParseResult* result, const char* s);
int yydebug=1;

%}


%defines
%pure-parser
%name_prefix="Calc"
%error_verbose
%verbose
%lex-param {yyscan_t scanner}
%lex-param {CalcParseResult* result}
%parse-param {yyscan_t scanner}
%parse-param {CalcParseResult* result}

%union {
	int	int_value;
	double	double_value;
}

%token <double_value>		DOUBLE_LITERAL
%token ADD SUB MUL DIV CR
%type <double_value> expression term primary_expression

%%

line_list
	: line
	| line_list line
;
line
	: expression CR
	{
        result->ret = $1;
	}
;
expression
	: term
	| expression ADD term
	{
		$$ = $1 + $3;
	}
	| expression SUB term
	{
		$$ = $1 - $3;
	}
;
term
	: primary_expression
	| term MUL primary_expression
	{
		$$ = $1 * $3;
	}
	| term DIV primary_expression
	{
		$$ = $1 / $3;
	}
;
primary_expression
	: DOUBLE_LITERAL
;

%%
