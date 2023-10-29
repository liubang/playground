#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calc.h"
#include "calc_flex.h"

extern int Calcparse(yyscan_t scanner, CalcResult* result);

#define NHASH 9997
static struct Symbol symtab[NHASH];

static unsigned SymHash(char* sym) {
    unsigned int hash = 0;
    unsigned c;
    while ((c = *sym++))
        hash = hash * 9 ^ c;
    return hash;
}

struct Symbol* Lookup(CalcResult* result, char* sym) {
    struct Symbol* sp = &symtab[SymHash(sym) % NHASH];
    int scount = NHASH;
    while (--scount >= 0) {
        if (sp->name && !strcmp(sp->name, sym)) {
            return sp;
        }

        if (!sp->name) {
            sp->name = strdup(sym);
            sp->value = 0;
            sp->func = NULL;
            sp->syms = NULL;
            return sp;
        }

        if (++sp >= symtab + NHASH)
            sp = symtab;
    }
    Calcerror("symbol table overflow", result, NULL);
    abort();
}

struct Ast* NewAst(CalcResult* result, int nodetype, struct Ast* l, struct Ast* r) {
    struct Ast* a = malloc(sizeof(struct Ast));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = nodetype;
    a->l = l;
    a->r = r;
    return a;
}

struct Ast* NewNum(CalcResult* result, double d) {
    struct NumVal* a = malloc(sizeof(struct NumVal));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = 'K';
    a->number = d;
    return (struct Ast*)a;
}

struct Ast* NewCmp(CalcResult* result, int cmptype, struct Ast* l, struct Ast* r) {
    struct Ast* a = malloc(sizeof(struct Ast));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = '0' + cmptype;
    a->l = l;
    a->r = r;
    return a;
}

struct Ast* NewFunc(CalcResult* result, int functype, struct Ast* l) {
    struct FnCall* a = malloc(sizeof(struct FnCall));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = 'F';
    a->l = l;
    a->functype = functype;
    return (struct Ast*)a;
}

struct Ast* NewCall(CalcResult* result, struct Symbol* s, struct Ast* l) {
    struct UfnCall* a = malloc(sizeof(struct UfnCall));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = 'C';
    a->l = l;
    a->s = s;
    return (struct Ast*)a;
}

struct Ast* NewRef(CalcResult* result, struct Symbol* s) {
    struct SymRef* a = malloc(sizeof(struct SymRef));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = 'N';
    a->s = s;
    return (struct Ast*)a;
}

struct Ast* NewAssign(CalcResult* result, struct Symbol* s, struct Ast* v) {
    struct SymAssign* a = malloc(sizeof(struct SymAssign));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = '=';
    a->s = s;
    a->v = v;
    return (struct Ast*)a;
}

struct Ast*
NewFlow(CalcResult* result, int nodetype, struct Ast* cond, struct Ast* tl, struct Ast* el) {
    struct Flow* a = malloc(sizeof(struct Flow));
    if (!a) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    a->nodetype = nodetype;
    a->cond = cond;
    a->tl = tl;
    a->el = el;
    return (struct Ast*)a;
}

void TreeFree(struct Ast* a) {
    switch (a->nodetype) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case 'L':
        TreeFree(a->r);
    case '|':
    case 'M':
    case 'C':
    case 'F':
        TreeFree(a->l);
    case 'K':
    case 'N':
        break;
    case '=':
        free(((struct SymAssign*)a)->v);
        break;
    case 'I':
    case 'W':
        free(((struct Flow*)a)->cond);
        if (((struct Flow*)a)->tl)
            TreeFree(((struct Flow*)a)->tl);
        if (((struct Flow*)a)->el)
            TreeFree(((struct Flow*)a)->el);
        break;
    default:
        printf("internal error: free bad node %c\n", a->nodetype);
    }
    free(a);
}

struct SymList* NewSymList(CalcResult* result, struct Symbol* sym, struct SymList* next) {
    struct SymList* sl = malloc(sizeof(struct SymList));
    if (!sl) {
        Calcerror("out of space", result, NULL);
        exit(0);
    }
    sl->sym = sym;
    sl->next = next;
    return sl;
}

void SymListFree(struct SymList* sl) {
    struct SymList* nsl;

    while (sl) {
        nsl = sl->next;
        free(sl);
        sl = nsl;
    }
}

static double callbuiltin(struct FnCall*);
static double calluser(struct UfnCall*);

double Eval(struct Ast* a) {
    double v = 0.0;
    if (!a) {
        printf("internal error, null eval\n");
        return 0.0;
    }

    switch (a->nodetype) {
    /* constant */
    case 'K':
        v = ((struct NumVal*)a)->number;
        break;
    /* symbol ref */
    case 'N':
        v = ((struct SymRef*)a)->s->value;
        break;
    case '=':
        v = ((struct SymAssign*)a)->s->value = Eval(((struct SymAssign*)a)->v);
        break;
    /* expression */
    case '+':
        v = Eval(a->l) + Eval(a->r);
        break;
    case '-':
        v = Eval(a->l) - Eval(a->r);
        break;
    case '*':
        v = Eval(a->l) * Eval(a->r);
        break;
    case '/':
        v = Eval(a->l) / Eval(a->r);
        break;
    case '|':
        v = fabs(Eval(a->l));
        break;
    case 'M':
        v = -Eval(a->l);
        break;
    /* comparisions */
    case '1':
        v = (Eval(a->l) > Eval(a->r)) ? 1 : 0;
        break;
    case '2':
        v = (Eval(a->l) < Eval(a->r)) ? 1 : 0;
        break;
    case '3':
        v = (Eval(a->l) != Eval(a->r)) ? 1 : 0;
        break;
    case '4':
        v = (Eval(a->l) == Eval(a->r)) ? 1 : 0;
        break;
    case '5':
        v = (Eval(a->l) >= Eval(a->r)) ? 1 : 0;
        break;
    case '6':
        v = (Eval(a->l) <= Eval(a->r)) ? 1 : 0;
        break;
    /* control flow */
    case 'I':
        if (Eval(((struct Flow*)a)->cond) != 0) {
            if (((struct Flow*)a)->tl) {
                v = Eval(((struct Flow*)a)->tl);
            } else {
                v = 0.0;
            }
        } else {
            if (((struct Flow*)a)->el) {
                v = Eval(((struct Flow*)a)->el);
            } else {
                v = 0.0;
            }
        }
        break;
    case 'W':
        v = 0.0;
        if (((struct Flow*)a)->tl) {
            while (Eval(((struct Flow*)a)->cond) != 0) {
                v = Eval(((struct Flow*)a)->tl);
            }
        }
        break;
    case 'L':
        Eval(a->l);
        v = Eval(a->r);
        break;
    case 'F':
        v = callbuiltin((struct FnCall*)a);
        break;
    case 'C':
        v = calluser((struct UfnCall*)a);
        break;
    default:
        printf("internal error: bad node %c\n", a->nodetype);
    }
    return v;
}

static double callbuiltin(struct FnCall* f) {
    enum Bifs functype = f->functype;
    double v = Eval(f->l);
    switch (functype) {
    case B_sqrt:
        return sqrt(v);
    case B_exp:
        return exp(v);
    case B_log:
        return log(v);
    case B_print:
        printf("= %4.4g\n", v);
        return v;
    default:
        printf("Unknown built-in function %d", functype);
        return 0.0;
    }
}

void DoDef(CalcResult* result, struct Symbol* name, struct SymList* syms, struct Ast* func) {
    if (name->syms)
        SymListFree(name->syms);
    if (name->func)
        TreeFree(name->func);
    name->syms = syms;
    name->func = func;
}

static double calluser(struct UfnCall* f) {
    struct Symbol* fn = f->s; /* function name */
    struct SymList* sl;       /* dummy args */
    struct Ast* args = f->l;  /* real args */
    double *oldval, *newval;  /* saved arg values */
    double v;
    int nargs;
    int i;
    if (!fn->func) {
        printf("call to undefined function %s", fn->name);
        return 0;
    }
    sl = fn->syms;
    for (nargs = 0; sl; sl = sl->next)
        nargs++;

    oldval = (double*)malloc(nargs * sizeof(double));
    newval = (double*)malloc(nargs * sizeof(double));
    if (!oldval || !newval) {
        if (oldval)
            free(oldval);
        if (newval)
            free(newval);
        printf("out of space in %s", fn->name);
        return 0.0;
    }

    for (i = 0; i < nargs; ++i) {
        if (!args) {
            printf("too few args in call to %s", fn->name);
            free(oldval);
            free(newval);
            return 0.0;
        }
        if (args->nodetype == 'L') {
            newval[i] = Eval(args->l);
            args = args->r;
        } else {
            newval[i] = Eval(args);
            args = NULL;
        }
    }

    sl = fn->syms;

    for (i = 0; i < nargs; ++i) {
        struct Symbol* s = sl->sym;
        oldval[i] = s->value;
        s->value = newval[i];
        sl = sl->next;
    }

    free(newval);
    v = Eval(fn->func);

    sl = fn->syms;
    for (i = 0; i < nargs; ++i) {
        struct Symbol* s = sl->sym;
        s->value = oldval[i];
        sl = sl->next;
    }
    free(oldval);
    return v;
}

CalcResult* CalcParse(const char* sql) {
    int ret = 0;
    FILE* fin = fmemopen((void*)sql, strlen(sql), "r");
    CalcResult* result = NewCalcResult();

    yyscan_t scanner;
    Calclex_init(&scanner);
    Calcset_in(fin, scanner);
    ret = Calcparse(scanner, result);
    Calclex_destroy(scanner);
    fclose(fin);
    if (ret) {
        printf("oops!!!\n");
        result->is_error = 1;
    }
    return result;
}

CalcResult* NewCalcResult() {
    CalcResult* result = (CalcResult*)malloc(sizeof(CalcResult));
    result->error_msg = NULL;
    return result;
}

void DestroyCalcResult(CalcResult* result) {
    if (result) {
        if (result->error_msg) {
            free(result->error_msg);
        }
        if (result->ast) {
            TreeFree(result->ast);
        }
        free(result);
    }
}
