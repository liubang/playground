%token_prefix TK_

%include {
#include <stdio.h>
#include "types.h"
#include "ttokens.h"
}

%syntax_error {
    fprintf(stderr, "Syntax error\n");    
}

%token_type { struct Token* }
%type expr { int }

%left PLUS MINUS.
%left TIMES DIVIDE.

program ::= expr(A). { printf("Result = %d\n", A); }
expr(A) ::= expr(B) PLUS expr(C). { A = B + C; }
expr(A) ::= expr(B) MINUS expr(C). { A = B - C; }
expr(A) ::= expr(B) TIMES expr(C). { A = B * C; }
expr(A) ::= expr(B) DIVIDE expr(C). { 
    if (C != 0) {
        A = B / C;
    } else {
        fprintf(stderr, "Divide by zero.");
    }
}
expr(A) ::= LPAR expr(B) RPAR. { A = B; }
expr(A) ::= INTEGER(B). { A = B->value; printf("passed argument: %s\n", B->token); }
