#include <stdio.h>

extern int yyparse(void);

int main(int argc, char* argv[]) {
    printf("> ");
    return yyparse();
}

// #include <stdio.h>
//
// extern int yylineno;
// extern int yylex(void);
// extern FILE *yyin;
// extern char *yytext;
// extern int yyparse(void);
//
// int yyerror(char const *str) {
//   fprintf(stderr, "syntax error near %s\n", yytext);
//   return 0;
// }
//
// int main(int argc, char *argv[]) {
//   yyin = stdin;
//   if (yyparse()) {
//     fprintf(stderr, "Core Dump!\n");
//     return -1;
//   }
//   return 0;
// }
