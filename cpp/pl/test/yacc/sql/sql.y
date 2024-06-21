%{
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

void yyerror(char *s, ...);
void emit(char *s, ...);
%}

%union {
    int intval;
    double floatval;
    char *strval;
    int subtok;
}

/* names and literal values */
%token <strval> NAME
%token <strval> STRING
%token <intval> INTNUM
%token <intval> BOOL
%token <floatval> APPROXNUM
/* user @abc names */
%token <strval> USERVAR

/* operators and precedence levels */
%right ASSIGN
%left OR
%left XOR
%left ANDOP
%nonassoc IN IS LIKE REGEXP
%left NOT '!'
%left BETWEEN
%left <subtok> COMPARISON /* = <> < > <= >= <=> */
%left '|'
%left '&'
%left <subtok> SHIFT /* << >> */
%left '+' '-'
%left '*' '/' '%' MOD
%left '^'
%nonassoc UMINUS

%token ADD
%token ALL
%token ESCAPED
%token <subtok> EXISTS /* NOT EXISTS or EXISTS */
%token FSUBSTRING
%token FTRIM
%token FDATE_ADD FDATE_SUB
%token FCOUNT

%type <intval> select_opts select_expr_list
%type <intval> val_list opt_val_list case_list
%type <intval> groupby_list opt_with_rollup opt_asc_desc
%type <intval> table_references opt_inner_cross opt_outer
%type <intval> left_or_right opt_left_or_right_outer column_list
%type <intval> index_list opt_for_join
%type <intval> delete_opts delete_list
%type <intval> insert_opts insert_vals insert_vals_list
%type <intval> insert_asgn_list opt_if_not_exists update_opts update_asgn_list
%type <intval> opt_temporary opt_length opt_binary opt_uz enum_list
%type <intval> column_atts data_type opt_ignore_replace create_col_list
%start stmt_list

%%

$accept: stmt_list $end

stmt_list: stmt ';'
    | stmt_list ';'
    | error ';'
    | stmt_list error ';'
    ;

stmt: select_stmt;

select_stmt: SELECT select_opts select_expr_list
    | SELECT select_opts select_expr_list FROM table_references opt_where
             opt_groupby opt_having opt_orderby opt_limit opt_into_list

opt_where: /* empty */
    | WHERE expr

opt_groupby: /* empty */
    | GROUP BY groupby_list opt_with_rollup

groupby_list: expr opt_asc_desc
    |  groupby_list ',' expr opt_asc_desc

opt_asc_desc: /* empty */
    | ASC 
    | DESC

opt_with_rollup: /* empty */
    | WITH ROLLUP
