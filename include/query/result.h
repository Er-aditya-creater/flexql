#pragma once
/*
 * include/query/result.h
 * ----------------------
 * QueryResult - the structured output of every Engine::execute() call.
 */
#include <string>
#include <vector>

struct QueryResult {
    bool        isSelect = false;
    bool        ok       = true;
    std::string message;
    std::vector<std::string>              colNames;
    std::vector<std::vector<std::string>> rows;
};
