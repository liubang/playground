#pragma once

struct symbol {
  char *name;
  double value;
  struct ast *func;     /* stmt for the function */
  struct symlist *syms; /* list of dummy args */
};

struct symbol *lookup(char *);

struct symlist {
  struct symbol *sym;
  struct symlist *next;
};

struct symlist *newsymlist(struct symbol *sym, struct symlist *next);
void symlistfree(struct symlist *sl);

enum bifs {
  B_sqrt = 1,
  B_exp = 2,
  B_log = 3,
  B_print = 4,
};

struct ast {
  int nodetype;
  struct ast *l;
  struct ast *r;
};

struct fncall {
  int nodetype;  // 'F'
  struct ast *l;
  enum bifs functype;
};

struct ufncall {
  int nodetype;  // 'C'
  struct ast *l;
  struct symbol *s;
};

struct flow {
  int nodetype;  // 'I' or 'W'
  struct ast *cond;
  struct ast *tl;
  struct ast *el;
};

struct numval {
  int nodetype;  // 'K'
  double number;
};

struct symref {
  int nodetype;  // 'N'
  struct symbol *s;
};

struct symasgn {
  int nodetype;      // '='
  struct symbol *s;  // symbol
  struct ast *v;     // value
};

// build an AST
struct ast *newast(int nodetype, struct ast *l, struct ast *r);
struct ast *newcmp(int cmptype, struct ast *l, struct ast *r);
struct ast *newfunc(int functype, struct ast *l);
struct ast *newcall(struct symbol *s, struct ast *l);
struct ast *newref(struct symbol *s);
struct ast *newasgn(struct symbol *s, struct ast *v);
struct ast *newnum(double d);
struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl,
                    struct ast *tr);

void dodef(struct symbol *name, struct symlist *syms, struct ast *func);
double eval(struct ast *);
void treefree(struct ast *);

// interface to the lexer
void yyerror(char *s, ...);
