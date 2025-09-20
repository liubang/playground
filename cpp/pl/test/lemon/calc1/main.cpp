#include "ttokens.h"
#include "types.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    int value;
    void* pParser;
    const char* c;
    size_t i = 0;
    struct Token v[argc];
    if (2 > argc) {
        printf("Usage: %s <expression> \n", argv[0]);
        return 1;
    }
    pParser = (void*)ParseAlloc(malloc);

    for (i = 1; i < argc; ++i) {
        c = argv[i];
        v[i].token = c;
        switch (*c) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                for (value = 0; *c && *c >= '0' && *c <= '9'; c++) {
                    value = value * 10 + (*c - '0');
                }
                v[i].value = value;
                Parse(pParser, TK_INTEGER, &v[i]);
                break;
            case '+':
                Parse(pParser, TK_PLUS, NULL);
                break;
            case '-':
                Parse(pParser, TK_MINUS, NULL);
                break;
            case '*':
                Parse(pParser, TK_TIMES, NULL);
                break;
            case '/':
                Parse(pParser, TK_DIVIDE, NULL);
                break;
            case '(':
                Parse(pParser, TK_LPAR, NULL);
                break;
            case ')':
                Parse(pParser, TK_RPAR, NULL);
                break;
            default:
                fprintf(stderr, "Unexpected token %s\n", c);
        }
    }

    Parse(pParser, 0, NULL);
    ParseFree(pParser, free);

    return 0;
}
