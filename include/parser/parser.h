#pragma once
/*
 * include/parser/parser.h
 * -----------------------
 * TokenStream helper, Condition and JoinCondition structs,
 * and their parsing function declarations.
 */
#include "lexer.h"
#include "../common/types.h"
#include <stdexcept>

// ── Token stream ─────────────────────────────────────────────────────────────
struct TokenStream {
    std::vector<Token> tokens;
    std::size_t        pos = 0;

    Token& peek()    { return tokens[pos]; }
    Token  consume() { return tokens[pos++]; }
    bool   atEnd()   const { return tokens[pos].type == TT::END; }
    bool   check(TT t) const { return tokens[pos].type == t; }

    bool checkKeyword(const std::string& kw) const {
        return tokens[pos].type == TT::KEYWORD && tokens[pos].value == kw;
    }
    Token expect(TT t, const std::string& desc) {
        if (tokens[pos].type != t)
            throw std::runtime_error("Expected " + desc + ", got '" + tokens[pos].value + "'");
        return tokens[pos++];
    }
    Token expectKeyword(const std::string& kw) {
        if (tokens[pos].type != TT::KEYWORD || tokens[pos].value != kw)
            throw std::runtime_error("Expected keyword '" + kw + "', got '" + tokens[pos].value + "'");
        return tokens[pos++];
    }
    std::string expectName() {
        if (tokens[pos].type == TT::IDENT || tokens[pos].type == TT::KEYWORD)
            return tokens[pos++].value;
        throw std::runtime_error("Expected a name, got '" + tokens[pos].value + "'");
    }
};

// ── WHERE condition (single condition only) ───────────────────────────────────
struct Condition {
    std::string colRef;   // "table.col"  or  "col"
    std::string op;       // =  !=  <  >  <=  >=
    CellValue   value;
};

// ── JOIN ON:  leftTable.leftCol = rightTable.rightCol ─────────────────────────
struct JoinCondition {
    std::string leftTable,  leftCol;
    std::string rightTable, rightCol;
};

Condition     parseCondition   (TokenStream& ts);
JoinCondition parseJoinCondition(TokenStream& ts);
