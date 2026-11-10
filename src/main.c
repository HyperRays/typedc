#include <stdio.h>
//#define RETURN_PLAIN_TOKENS
#include "parser.h"
//#include "lexer.h"


int main(void) {
    const char *source =
        "a++;";
    Parser parser = parser_new(source);
    Expr* expr = parse_statement(&parser);
    if(!expr) {
        printf("%s\n",ParseErr_map[parser_error(&parser)]);
        debug("Origin", parser.token);
        abort();
    }
    print_expr(expr, 0);

    return 0;
}
