#pragma once
/*
 * include/query/engine.h
 * ----------------------
 * SQL Engine — owns the in-memory Database, LRU cache, and StorageManager.
 * All writes go to WAL before memory; checkpoint runs every 50k ops.
 */
#include "../common/types.h"
#include "../query/result.h"
#include "../cache/lru_cache.h"
#include "../parser/parser.h"
#include "../storage/storage.h"
#include <string>
#include <atomic>

class Engine {
    Database        db_;
    LRUCache        cache_;
    StorageManager  storage_;
    std::atomic<uint64_t> opCount_{0};
    static constexpr uint64_t CHECKPOINT_INTERVAL = 50000; // ops between checkpoints

    static ColType   parseColType(TokenStream& ts);
    static CellValue buildCell(const Token& tok, ColType ct, const std::string& col);

    void doCreate   (TokenStream& ts, QueryResult& qr);
    void doInsert   (TokenStream& ts, QueryResult& qr);
    void doSelect   (TokenStream& ts, QueryResult& qr);
    void doDelete   (TokenStream& ts, QueryResult& qr);
    void doInnerJoin(TokenStream& ts, const std::string& leftName,
                     bool selectAll,
                     const std::vector<std::string>& reqCols,
                     QueryResult& qr);

    void maybeCheckpoint();

public:
    Engine();
    QueryResult execute(const std::string& rawQuery);
    void        forceCheckpoint();
};
