#include <stdio.h>

int main(int argc, char *argv[]) {
  extern int yyparse(void);

  printf("> ");
  return yyparse();
}
