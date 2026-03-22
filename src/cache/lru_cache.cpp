/*
 * src/cache/lru_cache.cpp
 */
#include "../../include/cache/lru_cache.h"
#include <algorithm>
#include <cctype>

LRUCache::LRUCache(std::size_t cap) : capacity_(cap) {}

void LRUCache::put(const std::string& key,
                   const std::vector<std::string>& cols,
                   const std::vector<std::vector<std::string>>& rows)
{
    auto it = map_.find(key);
    if (it != map_.end()) { order_.erase(it->second.second); map_.erase(it); }
    else if (map_.size() >= capacity_) { map_.erase(order_.back()); order_.pop_back(); }
    order_.push_front(key);
    map_[key] = {{cols, rows}, order_.begin()};
}

bool LRUCache::get(const std::string& key,
                   std::vector<std::string>& cols,
                   std::vector<std::vector<std::string>>& rows)
{
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    order_.erase(it->second.second);
    order_.push_front(key);
    it->second.second = order_.begin();
    cols = it->second.first.colNames;
    rows = it->second.first.rows;
    return true;
}

// Evict all cached queries that reference tableName.
// A simple case-insensitive substring search on the SQL key is sufficient
// because table names are unique within a database and appear in every
// SELECT that touches that table.
void LRUCache::invalidateTable(const std::string& tableName) {
    // Build upper-case version of the table name for case-insensitive match
    std::string upper = tableName;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    std::vector<std::string> toErase;
    for (auto& [k, _] : map_) {
        std::string ku = k;
        std::transform(ku.begin(), ku.end(), ku.begin(), ::toupper);
        if (ku.find(upper) != std::string::npos)
            toErase.push_back(k);
    }
    for (const auto& k : toErase) {
        auto it = map_.find(k);
        if (it != map_.end()) {
            order_.erase(it->second.second);
            map_.erase(it);
        }
    }
}

void LRUCache::clear() { map_.clear(); order_.clear(); }
