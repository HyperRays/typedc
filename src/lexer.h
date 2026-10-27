#pragma once
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>


static const char *const punctuators[] = {
    // Three-character
    "<<=", ">>=", "...",

    // Two-character
    "->", "++", "--",
    "<<", ">>",
    "<=", ">=", "==", "!=",
    "&&", "||",
    "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=",
    "::",// Language extension

    // Single-charachter
    "+","-","*","/","=",
    ";",":",
    ",",
    "(",")",
    "{","}",
    "[","]",
    "<",">",
    "!","&","|",".","%",
    "~","^","?",
    "@" // Language extension
};

static const size_t punctuators_size = sizeof(punctuators) / sizeof(punctuators[0]);

static const char *const keywords[] = {
    "auto", "break", "case", "char",
    "const", "continue", "default", "do",
    "double", "else", "enum", "extern",
    "float", "for", "goto", "if",
    "inline", "int", "long", "register",
    "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch",
    "typedef", "union", "unsigned", "void",
    "volatile", "while",
    "_Alignas","_Alignof",
    "_Atomic","_Generic","_Noreturn",
    "_Static_assert","_Thread_local",

    // language extensions
    "pub", "import", "trait", "impl",
    "raw_c", "globals"
};

static const size_t keywords_size = sizeof(keywords) / sizeof(keywords[0]);



// Token
typedef enum {
  TK_IDENT,   // Identifiers
  TK_PUNCT,   // Punctuators
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_CHAR,    // Charachter literals
  TK_NUM,     // Numeric literals
  TK_PP_NUM,  // Preprocessing numbers
  TK_WHITE,   // Whitespace
  TK_COMM,    // Comment
  TK_RAW,     // Raw embedded C code
  TK_EOF,     // End-of-file markers
  TK_ERR      // Unexpected token
} TokenKind;

static const char* TokenKind_name_map[] = {
    "TK_IDENT",
    "TK_PUNCT",
    "TK_KEYWORD",
    "TK_STR",
    "TK_CHAR",
    "TK_NUM",
    "TK_PP_NUM",
    "TK_WHITE",
    "TK_COMM",
    "TK_RAW",
    "TK_EOF",
    "TK_ERR"
};

// Lexing error hint
typedef enum {
    NO_ERR = 0,     // default no error
    OPEN_ML,        // multiline comment left open
    OPEN_LITERAL,   // string / charchter quote left open
    OPEN_RAW,       // open raw c code block
    MISSING_EP,     // exponent missing for hexadecimal
    MISSING_E,      // exponent digits missing
    UNEXPECTED,     // general catch all
} LexingError;

typedef struct Token {
    TokenKind kind;
    LexingError err;

    const char* start;
    size_t length;

    // Useful for helpful errors
    size_t row;
    size_t col;

} Token;

typedef struct {
    const char *source;
    size_t length;
    size_t pos;

    size_t row;
    size_t col;
} Lexer;

static inline Lexer lexer_new(const char* source) {
    return (Lexer){
        .source = source,
        .length = strlen(source),
        .pos = 0,
        .row = 1,
        .col = 1,
    };
}

static inline char lexer_peek(Lexer* lexer) {
    if (lexer->pos >= lexer->length)
        return '\0';

    return lexer->source[lexer->pos];
}

static inline char lexer_peek_at(Lexer* lexer, size_t offset) {
    if (lexer->pos + offset >= lexer->length)
        return '\0';

    return lexer->source[lexer->pos + offset];
}

static inline char lexer_advance(Lexer* lexer) {
    char c = lexer_peek(lexer);

    if (c == '\0') {
        return c;
    }

    lexer->pos++;

    if (c == '\n') {
        lexer->row++;
        lexer->col = 1;
    } else {
        lexer->col++;
    }

    return c;
}



static inline Token lexer_token(
    Lexer *lexer,
    TokenKind kind,
    size_t start,
    size_t row,
    size_t col
) {
    return (Token) {
        .kind = kind,
        .err = NO_ERR,
        .start = lexer->source + start,
        .length = lexer->pos - start,
        .row = row,
        .col = col,
    };
}

static inline Token lexer_token_err(
    Lexer *lexer,
    TokenKind kind,
    LexingError err,
    size_t start,
    size_t row,
    size_t col
) {
    Token token = lexer_token(lexer, kind, start, row, col);
    token.err = err;
    return token;
}


static inline size_t lexer_matcher(Lexer* lexer, const char* text) {
    size_t length = strlen(text);

    if (length > (lexer->length - lexer->pos))
        return 0;

    if (memcmp(lexer->source + lexer->pos, text, length) == 0) {
        return length;
    }
    return 0;
}

static inline int is_keyword(const char *start, size_t length) {
    for (size_t i = 0;
         i < keywords_size;
         i++) {

        size_t n = strlen(keywords[i]);

        if (length == n &&
            memcmp(start, keywords[i], n) == 0) {
            return 1;
        }
    }

    return 0;
}

static inline int token_is(Token token, const char *text) {
    size_t length = strlen(text);

    return token.length == length &&
           memcmp(token.start, text, length) == 0;
}

static inline Token lexer_next(Lexer* lexer) {

    for (;;) {
        // remember where the token starts (for whitespace & comments too)
        size_t start_wc = lexer->pos;
        size_t row_wc = lexer->row;
        size_t col_wc = lexer->col;

        // skip whitespace
        if (isspace((unsigned char)lexer_peek(lexer))) {
            while (isspace((unsigned char)lexer_peek(lexer))) {
                lexer_advance(lexer);
            }
            #ifdef RETURN_PLAIN_TOKENS
            return lexer_token(lexer, TK_WHITE, start_wc, row_wc, col_wc);
            #else
            continue;
            #endif
        }


        // Single-line comments
        if (lexer_matcher(lexer, "//")) {
            while (
                lexer_peek(lexer) != '\0' &&
                lexer_peek(lexer) != '\n'
            ) {
                lexer_advance(lexer);
            }
            #ifdef RETURN_PLAIN_TOKENS
            return lexer_token(lexer, TK_COMM, start_wc, row_wc, col_wc);
            #else
            continue;
            #endif
        }

        // Multiline comments
        if (lexer_matcher(lexer, "/*")) {
            while (
                lexer_peek(lexer) != '\0' &&
                lexer_matcher(lexer, "*/") == 0
            ) {
                lexer_advance(lexer);
            }
            if (lexer_matcher(lexer, "*/") == 0) {
                return lexer_token_err(lexer, TK_ERR, OPEN_ML, start_wc, row_wc, col_wc);
            }
            // flush "*/" symbol at the end
            lexer_advance(lexer);
            lexer_advance(lexer);

            #ifdef RETURN_PLAIN_TOKENS
            return lexer_token(lexer, TK_COMM, start_wc, row_wc, col_wc);
            #else
            continue;
            #endif

        }

        break;
    }

    // remember where the token starts
    size_t start = lexer->pos;
    size_t row = lexer->row;
    size_t col = lexer->col;

    char c = lexer_peek(lexer);

    // end of file
    if (c == '\0') {
        return lexer_token(lexer, TK_EOF, start, row, col);
    }

    // raw c code wrapped in a #{ }# block
    if (lexer_matcher(lexer, "#{")) {
        while (lexer_peek(lexer) != '\0' && !(lexer_matcher(lexer, "}#") && !lexer_matcher(lexer, "}#!") )) {
            lexer_advance(lexer);
        }
        if (!lexer_matcher(lexer, "}#")) {
            return lexer_token_err(lexer, TK_ERR, OPEN_RAW, start, row, col);
        }
        // flush #} symbol
        lexer_advance(lexer);
        lexer_advance(lexer);
        return lexer_token(lexer, TK_RAW, start, row, col);
    }


    // raw numeric literal (taken from chibicc)
    if (isdigit((unsigned char) c) || (c == '.' && isdigit((unsigned char)lexer_peek_at(lexer, 1)))) {
        lexer_advance(lexer);
        for (;;) {
            char c = lexer_peek(lexer);
            char n = lexer_peek_at(lexer, 1);
            if (c && n && strchr("eEpP", c) && strchr("+-", n)) {
                lexer_advance(lexer);
                lexer_advance(lexer);
            }
            else if (isalnum((unsigned char)c) || c == '.')
                lexer_advance(lexer);
            else
                break;
        }
      return lexer_token(lexer, TK_PP_NUM, start, row, col);
    }

    //
    // string and character literals
    //

    char n = lexer_peek_at(lexer, 1);
    // include string prefix
    if ((c == 'l' || c == 'L' || c == 'u' || c == 'U') && (n == '"' || n == '\'')) {
        lexer_advance(lexer);
    }

    n = lexer_peek_at(lexer, 2);
    if ((lexer_matcher(lexer, "u8") != 0) && (n == '"' || n == '\'')) {
        lexer_advance(lexer); // u
        lexer_advance(lexer); // 8
    }

    c = lexer_peek(lexer);

    if (c == '"' || c == '\'') {
        char quote = lexer_advance(lexer);

        while (lexer_peek(lexer) != '\0') {
            char current = lexer_advance(lexer);

            // Escape sequence
            if (current == '\\') {
                if (lexer_peek(lexer) != '\0') {
                    lexer_advance(lexer);
                }
                continue;
            }

            // Closing quote
            if (current == quote) {
                return lexer_token(
                    lexer,
                    quote == '"' ? TK_STR : TK_CHAR,
                    start, row, col
                );
            }

            // Unescaped newline
            if (current == '\n') {
                break;
            }
        }

        // Reached EOF or an invalid newline before closing
        return lexer_token_err(
            lexer, TK_ERR, OPEN_LITERAL, start, row, col
        );
    }

    // identifiers and kewords
    if (isalpha((unsigned char)c) || c == '_') {
        while (
            isalnum((unsigned char)lexer_peek(lexer)) ||
            lexer_peek(lexer) == '_'
        ) {
            lexer_advance(lexer);
        }

        Token token = lexer_token(
            lexer, TK_IDENT, start, row, col
        );

        if (is_keyword(token.start, token.length)) {
            token.kind = TK_KEYWORD;
        }

        return token;
    }



    // punctuation (multicharacter)
    for (size_t i = 0;
         i < punctuators_size;
         i++) {

        const char* p = punctuators[i];
        size_t n = lexer_matcher(lexer, p);

        if (n != 0) {
            for (size_t i = 0; i < n; i++)
                lexer_advance(lexer);

            return lexer_token(
                lexer, TK_PUNCT, start, row, col
            );
        }
    }

    // Unexpected character
    lexer_advance(lexer);

    return lexer_token_err(
        lexer, TK_ERR, UNEXPECTED, start, row, col
    );
}
