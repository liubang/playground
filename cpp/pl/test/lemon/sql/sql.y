%token_prefix TK_
%token_type { SToken }
%default_type { SNode* }
%default_destructor { nodesDestroyNode($$); }

%extra_argument { SAstCreateContext* pCxt }


%syntax_error {
    if (TOKEN.s[0]) {
        fprintf(stderr, "Near %d: syntax error", TOKEN.n);
    } else {
        fprintf(stderr, "Incomplete input.")
    }
}

%stack_overflow {
    fprintf(stderr, "Parser stack overflow.")
}

%parse_failure {
    fprintf(stderr, "Giving up.  Parser is hopelessly lost...\n");
}


%include {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "typedef.h"

#define YYSTACKDEPTH 0
}

%left OR.
%left AND.
%left UNION ALL MINUS EXCEPT INTERSECT.
%left NK_BITAND NK_BITOR NK_LSHIFT NK_RSHIFT.
%left NK_PLUS NK_MINUS.
%left NK_STAR NK_SLASH NK_REM.
%left NK_CONCAT.


