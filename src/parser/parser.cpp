/*
 * src/parser/parser.cpp
 */
#include "../../include/parser/parser.h"

// Helper: parse "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DD" -> Unix timestamp
static int64_t parseDatetime(const std::string& s) {
    struct tm tm_s = {};
    const char* ok = strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm_s);
    if (!ok) ok = strptime(s.c_str(), "%Y-%m-%d", &tm_s);
    if (!ok) return 0;
    tm_s.tm_isdst = -1;
    return (int64_t)mktime(&tm_s);
}

Condition parseCondition(TokenStream& ts) {
    Condition c;
    std::string first = ts.expectName();
    if (ts.check(TT::DOT)) { ts.consume(); c.colRef = first + "." + ts.expectName(); }
    else c.colRef = first;

    Token op = ts.consume();
    switch (op.type) {
        case TT::OP_EQ:  c.op = "=";  break;
        case TT::OP_NEQ: c.op = "!="; break;
        case TT::OP_LT:  c.op = "<";  break;
        case TT::OP_GT:  c.op = ">";  break;
        case TT::OP_LTE: c.op = "<="; break;
        case TT::OP_GTE: c.op = ">="; break;
        default: throw std::runtime_error("Expected comparison operator, got '" + op.value + "'");
    }

    Token val = ts.consume();
    if (val.type == TT::NUMBER) {
        if (val.value.find('.') != std::string::npos) c.value = std::stod(val.value);
        else                                          c.value = (int64_t)std::stoll(val.value);
    } else if (val.type == TT::DATETIME_LIT) {
        c.value = (int64_t)parseDatetime(val.value);
    } else if (val.type == TT::STRING_LIT || val.type == TT::IDENT) {
        c.value = val.value;
    } else {
        throw std::runtime_error("Expected a value after operator, got '" + val.value + "'");
    }
    return c;
}

JoinCondition parseJoinCondition(TokenStream& ts) {
    JoinCondition jc;
    jc.leftTable  = ts.expectName(); ts.expect(TT::DOT, "."); jc.leftCol  = ts.expectName();
    ts.expect(TT::OP_EQ, "=");
    jc.rightTable = ts.expectName(); ts.expect(TT::DOT, "."); jc.rightCol = ts.expectName();
    return jc;
}
