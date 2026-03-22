#pragma once
/*
 * include/expiration/ttl.h
 * ------------------------
 * isExpired() - returns true when a row has passed its TTL.
 */
#include "../common/types.h"
#include <ctime>

inline bool isExpired(const Row& row) {
    return row.expires != 0 && std::time(nullptr) > row.expires;
}
