%{
#include <stdio.h>
#include <stdlib.h>

#define YY_DECL
#include "flux_lexer.h"
#include "flux.h"
#include "flux_parser.h"

%}

%defines
%pure-parser
%name_prefix="Flux"
%error_verbose
%verbose

%union {
    struct Ast *ast;
    double f; /* float lit */
    long i;   /* int lit */
    char* s;  /* string/duration lit */
}

%token IDENTIFIER INT_LIT FLOAT_LIT STRING_LIT DURATION_LIT PIPE_RECEIVE_LIT REGEX_LIT
%token AND IMPORT OPTION IF OR PACKAGE BUILTIN THEN NOT RETURN TESTCASE ELSE EXISTS
