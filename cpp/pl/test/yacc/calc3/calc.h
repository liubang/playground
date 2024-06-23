// Copyright (c) 2024 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <stdio.h>

typedef struct CalcResult {
    struct Ast* ast;
    int is_error;
    char* error_msg;
} CalcResult;

extern CalcResult* NewCalcResult();
extern void DestroyCalcResult(CalcResult* result);
extern CalcResult* CalcParse(const char* sql);

struct Symbol {
    char* name;
    double value;
    struct Ast* func;     /* stmt for the function */
    struct SymList* syms; /* list of dummy args */
};

struct SymList {
    struct Symbol* sym;
    struct SymList* next;
};

struct Symbol* Lookup(CalcResult*, char*);
struct SymList* NewSymList(CalcResult* result, struct Symbol* sym, struct SymList* next);
void SymListFree(struct SymList* sl);

enum Bifs {
    B_sqrt = 1,
    B_exp = 2,
    B_log = 3,
    B_print = 4,
};

struct Ast {
    int nodetype;
    struct Ast* l;
    struct Ast* r;
};

struct FnCall {
    int nodetype; // 'F'
    struct Ast* l;
    enum Bifs functype;
};

struct UfnCall {
    int nodetype; // 'C'
    struct Ast* l;
    struct Symbol* s;
};

struct Flow {
    int nodetype; // 'I' or 'W'
    struct Ast* cond;
    struct Ast* tl;
    struct Ast* el;
};

struct NumVal {
    int nodetype; // 'K'
    double number;
};

struct SymRef {
    int nodetype; // 'N'
    struct Symbol* s;
};

struct SymAssign {
    int nodetype;     // '='
    struct Symbol* s; // symbol
    struct Ast* v;    // value
};

// build an AST
struct Ast* NewAst(CalcResult* result, int nodetype, struct Ast* l, struct Ast* r);
struct Ast* NewCmp(CalcResult* result, int cmptype, struct Ast* l, struct Ast* r);
struct Ast* NewFunc(CalcResult* result, int functype, struct Ast* l);
struct Ast* NewCall(CalcResult* result, struct Symbol* s, struct Ast* l);
struct Ast* NewRef(CalcResult* result, struct Symbol* s);
struct Ast* NewAssign(CalcResult* result, struct Symbol* s, struct Ast* v);
struct Ast* NewNum(CalcResult* result, double d);
struct Ast* NewFlow(
    CalcResult* result, int nodetype, struct Ast* cond, struct Ast* tl, struct Ast* tr);

void DoDef(CalcResult* result, struct Symbol* name, struct SymList* syms, struct Ast* func);
double Eval(struct Ast*);
void TreeFree(struct Ast*);
