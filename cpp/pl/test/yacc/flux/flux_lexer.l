%{
#include <stdio.h>
#include "flux.h"
#include "flux_parser.h"
%}

%option nounput noyywrap nodefault yylineno reentrant
%option prefix="Flux"


%%

/* operators */
"+" |
"-" |
"*" |
"/" |
"=" |
"|" |
"," |
";" |
"[" |
"]" |
"{" |
"}" |
"(" |
")"     { return yytext[0]; }

/* compara operator */
">"  { yylval->fn = 1; return CMP; }
"<"  { yylval->fn = 2; return CMP; }
"<>" { yylval->fn = 3; return CMP; }
"==" { yylval->fn = 4; return CMP; }
">=" { yylval->fn = 5; return CMP; }
"<=" { yylval->fn = 6; return CMP; }


/* keywords */
"and"      { return AND; }
"import"   { return IMPORT; }
"option"   { return OPTION; }
"if"       { return IF; }
"or"       { return OR; }
"package"  { return PACKAGE; }
"builtin"  { return BUILTIN; }
"then"     { return THEN; }
"not"      { return NOT; }
"return"   { return RETURN; }
"testcase" { return TESTCASE; }
"else"     { return ELSE; }
"exists"   { return EXISTS; }

[ \r\t\n]+           /* ignore white char */
[a-zA-Z]+            { yylval->id = yytext; return IDENTIFIER; }
[0-9]+               { yylval->i = atoi(yytext); return INT_LIT; }
[0-9]+ '.' [0-9]+    { yylval->f = atof(yytext); return FLOAT_LIT; }
'"' [a-zA-Z0-9]+ '"' { yylval->s = yytext; return STRING_LIT; }
[0-9][0-9][0-9][0-9] '-' [0-9][0-9] '-' [0-9][0-9] 'T' [0-9][0-9] ':' [0-9][0-9] ':' [0-9][0-9] 'Z'
                     { yylval->s = yytext; return DURATION_LIT; }
'<-'                 { return PIPE_RECEIVE_LIT; }
'//'                 { return REGEX_LIT; }


%%
