#include <stdio.h>
//#define RETURN_PLAIN_TOKENS
#include "lexer.h"


int main(void) {
    const char *source =
        "#{ plain text #} float f = 0b1010210;";

    Lexer lexer = lexer_new(source);

    while (1) {
        Token token = lexer_next(&lexer);

        if (token.kind == TK_ERR) {
            printf(
                "%zu:%zu  kind=%d err=%d text='%.*s'\n",
                token.row,
                token.col,
                token.kind,
                token.err,
                (int)token.length,
                token.start
            );
            break;
        } else if (token.kind == TK_EOF) {
            printf(
                "%zu:%zu  kind=%d  text='%.*s'\n",
                token.row,
                token.col,
                token.kind,
                (int)token.length,
                token.start
            );
            break;
        } else {
            printf(
                "%zu:%zu  kind=%d  text='%.*s'\n",
                token.row,
                token.col,
                token.kind,
                (int)token.length,
                token.start
            );
        }
    }
    return 0;
}
