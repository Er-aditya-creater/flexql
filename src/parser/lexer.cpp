/*
 * src/parser/lexer.cpp
 */
#include "../../include/parser/lexer.h"
#include <algorithm>
#include <cctype>

static const char* KEYWORDS[] = {
    "CREATE","TABLE","INSERT","INTO","VALUES","SELECT","FROM","WHERE",
    "INNER","JOIN","ON","TTL",
    "INT","INTEGER","DECIMAL","FLOAT","DOUBLE","NUMERIC",
    "VARCHAR","TEXT","CHAR",
    "DATETIME","DATE","TIME",
    "PRIMARY","KEY","NOT","NULL",
    nullptr
};

static bool isKeyword(const std::string& s) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), ::toupper);
    for (int i = 0; KEYWORDS[i]; ++i)
        if (u == KEYWORDS[i]) return true;
    return false;
}

static std::string toUpper(const std::string& s) {
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), ::toupper);
    return u;
}

static bool looksLikeDatetime(const std::string& s) {
    if (s.size() < 10) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c) && c != '-' && c != ':' && c != ' ')
            return false;
    return true;
}

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    std::size_t i = 0, n = src.size();

    while (i < n) {
        if (std::isspace((unsigned char)src[i])) { ++i; continue; }

        if (src[i] == '\'') {
            ++i; std::string lit;
            while (i < n && src[i] != '\'') lit += src[i++];
            if (i < n) ++i;
            out.push_back({looksLikeDatetime(lit) ? TT::DATETIME_LIT : TT::STRING_LIT, lit});
            continue;
        }

        if (std::isdigit((unsigned char)src[i]) ||
            (src[i] == '-' && i+1 < n && std::isdigit((unsigned char)src[i+1]))) {
            std::string num;
            if (src[i] == '-') num += src[i++];
            while (i < n && (std::isdigit((unsigned char)src[i]) || src[i] == '.'))
                num += src[i++];
            out.push_back({TT::NUMBER, num}); continue;
        }

        if (std::isalpha((unsigned char)src[i]) || src[i] == '_') {
            std::string id;
            while (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
                id += src[i++];
            out.push_back(isKeyword(id) ? Token{TT::KEYWORD, toUpper(id)}
                                        : Token{TT::IDENT,   id});
            continue;
        }

        switch (src[i]) {
            case '*': out.push_back({TT::STAR,  "*"}); ++i; break;
            case ',': out.push_back({TT::COMMA, ","}); ++i; break;
            case '(': out.push_back({TT::LPAREN,"("}); ++i; break;
            case ')': out.push_back({TT::RPAREN,")"}); ++i; break;
            case ';': out.push_back({TT::SEMICOLON,";"}); ++i; break;
            case '.': out.push_back({TT::DOT,   "."}); ++i; break;
            case '=': out.push_back({TT::OP_EQ, "="}); ++i; break;
            case '!':
                if (i+1<n&&src[i+1]=='='){out.push_back({TT::OP_NEQ,"!="});i+=2;}else ++i;
                break;
            case '<':
                if (i+1<n&&src[i+1]=='='){out.push_back({TT::OP_LTE,"<="});i+=2;}
                else{out.push_back({TT::OP_LT,"<"});++i;}
                break;
            case '>':
                if (i+1<n&&src[i+1]=='='){out.push_back({TT::OP_GTE,">="});i+=2;}
                else{out.push_back({TT::OP_GT,">"});++i;}
                break;
            default: ++i; break;
        }
    }
    out.push_back({TT::END, ""});
    return out;
}
