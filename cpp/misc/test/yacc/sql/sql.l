%option noyywrap nodefault yylineno case-insensitive

%{
#include "mysql_bison.h"
#include <stdarg.h>
#include <string.h>

void yyerror(char *s, ...);

int oldstate;
%}

%x COMMENT
%s BTWMODE

%%

ADD                 { return ADD; }
ALL                 { return ALL; }
ALTER               { return ALTER; }
ANALYZE             { return ANALYZE; }

<BTWMODE>AND        { BEGIN INITIAL; return AND; }
AND                 { return ANDOP; }
ANY                 { return ANY; }
AS                  { return AS; }
ASC                 { return ASC; }
AUTO_INCREMENT      { return AUTO_INCREMENT; }
BEFORE              { return BEFORE; }
BETWEEN             { BEGIN BTWMODE; return BETWEEN; }
INT8|BIGINT         { return BIGINT; }
BINARY              { return BINARY; }
BIT                 { return BIT; }
BLOB                { return BLOB; }
BOTH                { return BOTH; }
BY                  { return BY; }
CALL                { return CALL; }
CASCADE             { return CASCADE; }
CASE                { return CASE; }
CHANGE              { return CHANGE; }
CHAR(ACTER)?        { return CHAR; }
CHECK               { return CHECK; }
COLLATE             { return COLLATE; }
COLUMN              { return COLUMN; }
COMMENT             { return COMMENT; }
CONDITION           { return CONDITION; }
CONSTRAINT          { return CONSTRAINT; }
CONTINUE            { return CONTINUE; }
CONVERT             { return CONVERT; }
CREATE              { return CREATE; }
CROSS               { return CROSS; }
CURRENT_DATE        { return CURRENT_DATE; }
CURRENT_TIME        { return CURRENT_TIME; }
CURRENT_TIMESTAMP   { return CURRENT_TIMESTAMP; }
CURRENT_USER        { return CURRENT_USER; }
CURSOR              { return CURSOR; }
DATABASE            { return DATABASE; }
DATABASES           { return DATABASES; }
DATE                { return DATE; }
DATATIME            { return DATETIME; }
DAY_HOUR            { return DAY_HOUR; }
DAY_MICROSECOND     { return DAY_MICROSECOND; }
DAY_MINUTE          { return DAY_MINUTE; }
DAY_SECOND          { return DAY_SECOND; }
NUMERIC|DEC|DECIMAL { return DECIMAL; }
DECLARE             { return DECLARE; }
DEFAULT             { return DEFAULT; }
DELAYED             { return DELAYED; }
DELETE              { return DELETE; }
DESC                { return DESC; }
DESCRIBE            { return DESCRIBE; }
DETERMINISTIC       { return DETERMINISTIC; }
DISTINCT            { return DISTINCT; }
DISTINCTROW         { return DISTINCTROW; }
DIV                 { return DIV; }
FLOAT8|DOUBLE       { return DOUBLE; }
DROP                { return DROP; }
DUAL                { return DUAL; }
EACH                { return EACH; }
ELSE                { return ELSE; }
ELSEIF              { return ELSEIF; }
END                 { return END; }
ENUM                { return ENUM; }
ESCAPED             { return ESCAPED; }
EXISTS              { yylval.subtok = 0; return EXISTS; }
NOT[ \t\n]+EXISTS   { yylval.subtok = 1; return EXISTS; }
EXIT                { return EXIT; }
EXPLAIN             { return EXPLAIN; }
FETCH               { return FETCH; }
FLOAT4?             { return FLOAT; }
FOR                 { return FOR; }
FORCE               { return FORCE; }
FOREIGN             { return FOREIGN; }
FROM                { return FROM; }
FULLTEXT            { return FULLTEXT; }
GRANT               { return GRANT; }
GROUP               { return GROUP; }
HAVING              { return HAVING; }
HIGH_PRIORITY       { return HIGH_PRIORITY; }
HOUR_MICROSECOND    { return HOUR_MICROSECOND; }
HOUR_MINUTE         { return HOUR_MINUTE; }
HOUR_SECOND         { return HOUR_SECOND; }
IF                  { return IF; }
IGNORE              { return IGNORE; }
IN                  { return IN; }
INFILE              { return INFILE; }
INNER               { return INNER; }
INOUT               { return INOUT; }
INSENSITIVE         { return INSENSITIVE; }
INSERT              { return INSERT; }
INT4?|INTEGER       { return INTEGER; }
INTERVAL            { return INTERVAL; }
INTO                { return INTO; }
IS                  { return IS; }
ITERATE             { return ITERATE; }
JOIN                { return JOIN; }
INDEX|KEY           { return KEY; }
KEYS                { return KEYS; }
KILL                { return KILL; }
LEADING             { return LEADING; }
LEAVE               { return LEAVE; }
LEFT                { return LEFT; }
LIKE                { return LIKE; }
LIMIT               { return LIMIT; }
LINES               { return LINES; }
LOAD                { return LOAD; }

%%
