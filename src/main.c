#include <stdio.h>
//#define RETURN_PLAIN_TOKENS
#include "parser.h"
//#include "lexer.h"


int main(void) {
    const char *source =
        "if (b) { b } ";

    Parser parser = parser_new(source);
    Expr* expr = parse_rule(&parser, if_rule);
    assert(expr);
    print_expr(expr, 0);

    return 0;
}
