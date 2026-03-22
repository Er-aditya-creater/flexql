#pragma once
/*
 * include/query/engine.h
 * ----------------------
 * SQL Engine - owns the in-memory Database and LRU cache.
 * execute() is the single entry point for all SQL commands.
 */
#include "../common/types.h"
#include "../query/result.h"
#include "../cache/lru_cache.h"
#include "../parser/parser.h"
#include <string>

class Engine {
    Database db_;
    LRUCache cache_;

    // Private helpers declared here, defined in engine.cpp
    static ColType  parseColType(TokenStream& ts);
    static CellValue buildCell(const Token& tok, ColType ct, const std::string& col);

    void doCreate   (TokenStream& ts, QueryResult& qr);
    void doInsert   (TokenStream& ts, QueryResult& qr);
    void doSelect   (TokenStream& ts, QueryResult& qr);
    void doInnerJoin(TokenStream& ts, const std::string& leftName,
                     bool selectAll,
                     const std::vector<std::string>& reqCols,
                     QueryResult& qr);
public:
    Engine() = default;
    QueryResult execute(const std::string& rawQuery);
};
