%{

#include <stdio.h>

int chars = 0;
int words = 0;
int lines = 0;

int yywrap(void)
{
    return 1;
}

%}

%%

[^ \t\n\r\f\v]+ { words++; chars += strlen(yytext); }
\n              { chars++; lines++; }
.               { chars++; }

%%

int main(int argc, char *argv[]) {
    yylex();
    printf("chars:%d, words:%d, lines:%d\n", chars, words, lines);
    return 0;
}
