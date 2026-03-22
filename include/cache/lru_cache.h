#pragma once
/*
 * include/cache/lru_cache.h
 * -------------------------
 * LRU cache for SELECT query results.
 *
 * Design:
 *   - std::list tracks access order (front = MRU).
 *   - unordered_map provides O(1) key lookup.
 *   - put(): store result, evict LRU entry when at capacity.
 *   - get(): return cached result, promote to MRU.
 *   - invalidateTable(): evict every entry whose SQL mentions a table.
 *     Called after every INSERT so TTL-expired rows are never served
 *     from stale cache entries.
 *
 * Capacity: 1024 entries by default.
 */
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <utility>

class LRUCache {
    struct Entry {
        std::vector<std::string>              colNames;
        std::vector<std::vector<std::string>> rows;
    };

    std::size_t    capacity_;
    std::list<std::string> order_;   // front = MRU
    std::unordered_map<std::string,
        std::pair<Entry, std::list<std::string>::iterator>> map_;

public:
    explicit LRUCache(std::size_t cap = 1024);

    void put(const std::string& key,
             const std::vector<std::string>&              cols,
             const std::vector<std::vector<std::string>>& rows);

    // Returns true on cache hit and fills cols/rows.
    bool get(const std::string& key,
             std::vector<std::string>&              cols,
             std::vector<std::vector<std::string>>& rows);

    // Evict every cached query whose SQL string mentions `tableName`.
    // Call this after any INSERT or CREATE that modifies `tableName`.
    void invalidateTable(const std::string& tableName);

    void        clear();
    std::size_t size() const { return map_.size(); }
};
