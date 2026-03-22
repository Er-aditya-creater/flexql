#pragma once
/*
 * include/parser/lexer.h
 * ----------------------
 * Token types and tokenize() declaration.
 */
#include <string>
#include <vector>

enum class TT {
    KEYWORD, IDENT, NUMBER, STRING_LIT, DATETIME_LIT,
    STAR, COMMA, LPAREN, RPAREN, SEMICOLON, DOT,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    END
};

struct Token { TT type; std::string value; };

// Tokenise a raw SQL string into a flat list of Tokens
std::vector<Token> tokenize(const std::string& src);
