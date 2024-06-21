%{
#include <stdio.h>
#include <stdlib.h>

#define YY_DECL
#include "calc_flex.h"
#include "calc.h"
#include "calc_bison.h"

extern int Calclex(YYSTYPE* yylval, yyscan_t yyscanner, CalcResult* result);
extern int Calcerror(yyscan_t scanner, CalcResult* result, const char* s);
int yydebug=1;
%}

%defines
%pure-parser
%name_prefix="Calc"
%error_verbose
%verbose
%lex-param {yyscan_t scanner}
%lex-param {CalcResult* result}
%parse-param {yyscan_t scanner}
%parse-param {CalcResult* result}

%union {
    struct Ast *a;
    double d;
    struct Symbol *s;
    struct SymList *sl;
    int fn;
}

/* declare tokens */
%token <d> NUMBER
%token <s> NAME
%token <fn> FUNC
%token EOL

%token IF THEN ELSE WHILE DO LET

%nonassoc <fn> CMP
%right '='
%left '+' '-'
%left '*' '/'
%nonassoc '|' UMINUS

%type <a> exp stmt list explist
%type <sl> symlist

%start calclist

%%

stmt: IF exp THEN list           { $$ = NewFlow(result, 'I', $2, $4, NULL); }
    | IF exp THEN list ELSE list { $$ = NewFlow(result, 'I', $2, $4, $6); }
    | WHILE exp DO list          { $$ = NewFlow(result, 'W', $2, $4, NULL); }
    | exp
    ;
list: { $$ = NULL; }
    | stmt ';' list 
    { 
        if ($3 == NULL) $$ = $1; 
        else $$ = NewAst(result, 'L', $1, $3); 
    }
    ;
exp: exp CMP exp           { $$ = NewCmp(result, $2, $1, $3); }
    | exp '+' exp          { $$ = NewAst(result, '+', $1, $3); }
    | exp '-' exp          { $$ = NewAst(result, '-', $1, $3); }
    | exp '*' exp          { $$ = NewAst(result, '*', $1, $3); }
    | exp '/' exp          { $$ = NewAst(result, '/', $1, $3); }
    | '|' exp              { $$ = NewAst(result, '|', $2, NULL); }
    | '(' exp ')'          { $$ = $2; }
    | '-' exp %prec UMINUS { $$ = NewAst(result, 'M', $2, NULL); }
    | NUMBER               { $$ = NewNum(result, $1); }
    | FUNC '(' explist ')' { $$ = NewFunc(result, $1, $3); }
    | NAME                 { $$ = NewRef(result, $1); }
    | NAME '=' exp         { $$ = NewAssign(result, $1, $3); }
    | NAME '(' explist ')' { $$ = NewCall(result, $1, $3); }
    ;
explist: exp 
    | exp ',' explist { $$ = NewAst(result, 'L', $1, $3); }
    ;
symlist: NAME { $$ = NewSymList(result, $1, NULL); }
    | NAME ',' symlist { $$ = NewSymList(result, $1, $3); }
    ;
calclist: /* empty */
    | calclist stmt EOL 
    { 
        result->ast = $2;
    }
    | calclist LET NAME '(' symlist ')' '=' list EOL 
    {
        DoDef(result, $3, $5, $8);
    }
    | calclist error EOL { 
        yyerrok; 
    }
    ;

%%
