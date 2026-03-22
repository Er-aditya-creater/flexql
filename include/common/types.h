#pragma once
/*
 * include/common/types.h
 * ----------------------
 * Core data types shared by every module.
 *
 * Storage mapping:
 *   INT      -> int64_t
 *   DECIMAL  -> double
 *   VARCHAR  -> std::string
 *   DATETIME -> int64_t  (Unix timestamp; ColType tag used for display)
 */
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <ctime>
#include <cstdint>

enum class ColType { INT, DECIMAL, VARCHAR, DATETIME };

using CellValue = std::variant<int64_t, double, std::string>;

struct Column { std::string name; ColType type; };

struct Row {
    std::vector<CellValue> cells;
    time_t                 expires = 0;  // 0 = never expires
};

struct Table {
    std::string         name;
    std::vector<Column> columns;
    std::vector<Row>    rows;
    // BST primary index: first-column display string -> row indices  O(log n)
    std::map<std::string, std::vector<std::size_t>> primaryIndex;
    // Set to true when any row has a TTL; disables caching for this table
    bool hasTTLRows = false;

    int colIndex(const std::string& n) const {
        for (int i = 0; i < (int)columns.size(); ++i)
            if (columns[i].name == n) return i;
        return -1;
    }
};

struct Database {
    std::map<std::string, Table> tables;
};
