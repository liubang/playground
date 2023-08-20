#pragma once

extern int yylineno;

void yyerror(char *s, ...);

struct ast {
  int nodetype;
  struct ast *l;
  struct ast *r;
};

struct numval {
  int nodetype;
  double number;
};

/* construct a ast */
struct ast *newast(int nodetype, struct ast *l, struct ast *r);
struct ast *newnum(double d);

double eval(struct ast *);
void treefree(struct ast *);
