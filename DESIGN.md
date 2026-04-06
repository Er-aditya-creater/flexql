# FlexQL — Design Document

## GitHub 

https://github.com/Er-aditya-creater/flexql

---

## 1. System Overview

FlexQL is a simplified SQL-like in-memory database driver implemented in C++17.
It follows a client-server architecture where the server owns all data and logic,
and the client provides an interactive REPL plus a C API for programmatic access.

---

## 2. Storage Design

### Row-Major Format

Data is stored in **row-major format**: each `Table` owns a `std::vector<Row>`,
where each `Row` is a `std::vector<CellValue>`.

**Why row-major?**
- `INSERT` is an append — O(1) amortised on `vector::push_back`.
- Full-table `SELECT` scans access all columns of one row together — row-major
  gives sequential memory access and good cache locality for this pattern.
- Column-major would improve aggregation queries (e.g. `SUM(price)`), but the
  assignment does not require aggregation, so row-major is the better fit.

### Schema Storage

Each `Table` holds a `std::vector<Column>` storing the column name and type.
The schema is checked on every `INSERT` and `SELECT`; unknown columns produce an
error at parse time, not at scan time.

### Column Types

| SQL type      | C++ storage type | Notes                                    |
|---------------|------------------|------------------------------------------|
| `INT`         | `int64_t`        | 64-bit signed integer                    |
| `DECIMAL`     | `double`         | IEEE 754 double precision                |
| `VARCHAR`     | `std::string`    | Arbitrary length string                  |
| `DATETIME`    | `int64_t`        | Unix timestamp; displayed as ISO-8601    |

`CellValue` is a `std::variant<int64_t, double, std::string>`. The `ColType`
tag on each column tells the display and comparison layer how to interpret an
`int64_t` (INT vs DATETIME).

---

## 3. Indexing

A **BST primary index** (`std::map<string, vector<size_t>>`) is maintained on
the **first column** of every table.

- The key is the display-string representation of the first-column cell value.
- The value is a list of row indices that share that key.
- The index is updated on every `INSERT` (O(log n) map insertion).
- When a `WHERE` clause targets the first column with `=`, the engine uses the
  BST index directly (O(log n)) instead of scanning all rows (O(n)).
- Any other `WHERE` condition falls back to a full sequential scan.

**Why `std::map` (BST)?**
- Supports ordered operations (`<`, `>`, `<=`, `>=`) naturally.
- O(log n) lookup is predictable and acceptable for the dataset sizes expected.
- An `unordered_map` (O(1) average) would be faster for equality-only lookups
  but cannot serve range queries without scanning all buckets.

---

## 4. INNER JOIN

The INNER JOIN uses a **hash-join algorithm**:

**Phase 1 — Build:** scan the right table and insert each row's join-column
value into a `std::unordered_map<string, vector<size_t>>` mapping to row indices.

**Phase 2 — Probe:** scan the left table; for each row, look up its join-column
value in the hash map. Emit a combined row for every match found.

**Why hash join?**
- Time complexity: **O(n + m)** average, where n and m are the row counts of
  the two tables. This is optimal for equi-joins.
- The previous implementation used a cross-join (O(n × m)), which is
  exponentially slower on large tables.
- Only rows with a matching join-column value on both sides are emitted — this
  is the correct INNER JOIN semantics.

An optional `WHERE` clause is applied after join-pair selection, before emitting
the row, so it does not affect the join algorithm.

---

## 5. Caching

An **LRU (Least Recently Used)** cache stores the results of `SELECT` queries.

**Data structure:**
- `std::list<string>` tracks access order; the front element is the Most
  Recently Used (MRU) entry, and the back is the Least Recently Used (LRU).
- `std::unordered_map<string, pair<Entry, list::iterator>>` provides O(1) key
  lookup and O(1) list-node access for promotion.

**Behaviour:**
- On every `SELECT`, the cache is checked **before** executing the query.
  A cache hit returns the stored result immediately without touching the database.
- On a cache miss, the query executes normally and the result is stored in the
  cache afterward.
- Capacity is 1024 entries. When full, the LRU entry is evicted on the next
  `put()`.

**Impact on performance:**
- Repeated identical `SELECT` queries are answered in O(1) time.
- The primary benefit is for read-heavy workloads where the same query recurs
  (e.g. a dashboard polling the same aggregation).

---

## 6. Expiration Timestamps (TTL)

Every `Row` carries a `time_t expires` field (0 = never expires).

When `INSERT INTO t VALUES (...) TTL <seconds>` is used, `expires` is set to
`time(nullptr) + seconds`.

On every read path (SELECT scan, JOIN probe, JOIN scan), `isExpired(row)` is
checked before processing the row. Expired rows are silently skipped; they
remain in the `rows` vector but are invisible to queries.

This is a **lazy expiry** strategy: expired rows are not deleted immediately
but are ignored during reads. This avoids write-locking the table on a timer
and keeps the read path simple.

---

## 7. Multithreading Design

The server spawns one **POSIX thread per client connection** using
`pthread_create` + `pthread_detach`.

All client threads share a single `Engine` instance. The `Engine` is protected
by a single `pthread_mutex_t g_mutex`. The `safeExecute()` function wraps every
call to `engine.execute()` with a `MutexGuard` RAII lock, ensuring:

- No two threads can read or write the database simultaneously.
- The mutex is always released even if `execute()` throws.

**Trade-off:** A single global mutex means threads serialize on the lock. For
the expected workload (query parsing is fast, dataset fits in RAM), this is
acceptable. A production system would use reader-writer locks (`pthread_rwlock_t`)
to allow concurrent reads.

---

## 8. Wire Protocol

The client and server communicate over TCP using a simple
**newline-terminated, tab-delimited text protocol**:

```
Client -> Server:   <sql>\n
                    QUIT\n

Server -> Client:   OK\t<message>\n
                    ERR\t<message>\n
                    COLS\t<n>\t<col1>\t<col2>...\n
                    ROW\t<val1>\t<val2>...\n
                    END\n
```

Tab characters inside cell values are replaced with spaces by `sanitize()`
before transmission to avoid protocol ambiguity.

---

## 9. File Structure

```
flexql/
├── Makefile
├── include/
│   ├── flexql.h              Public C API (opaque FlexQL handle)
│   ├── common/types.h        ColType, CellValue, Column, Row, Table, Database
│   ├── parser/lexer.h        Token types and tokenize() declaration
│   ├── parser/parser.h       TokenStream, Condition, JoinCondition
│   ├── query/engine.h        Engine class declaration
│   ├── query/result.h        QueryResult struct
│   ├── cache/lru_cache.h     LRUCache declaration
│   ├── network/protocol.h    Wire protocol constants
│   ├── expiration/ttl.h      isExpired() inline function
│   ├──storage/storage.h     Header file storae
│   └── concurrency/          MutexGuard RAII wrapper
│       mutex_guard.h
└── src/
    ├── parser/lexer.cpp      Tokeniser (INT, DECIMAL, VARCHAR, DATETIME)
    ├── parser/parser.cpp     Condition and JOIN condition parsing
    ├── cache/lru_cache.cpp   LRU cache implementation
    ├── query/engine.cpp      SQL engine (CREATE/INSERT/SELECT/INNER JOIN)
    ├── server/server.cpp     TCP server — no engine code
    ├──client/client.cpp     C API + REPL — no server code
    └── storage/storage.cpp   storage implementation
```

---

## 10. Compilation and Execution

```bash
# Build both binaries
make all bin/benchmark_flexql

# Start the server (default port 9000)
./bin/flexql-server 9000

# Connect an interactive client (in another terminal)
./bin/flexql-client 127.0.0.1 9000

# Run the automated test suite (server must be running)
make test

# Run insert benchmark (adjust row count)
./bin/benchmark_flexql 1000000
```

---

## 11. Performance Notes

| Operation                     | Complexity  | Notes                               |
|-------------------------------|-------------|-------------------------------------|
| INSERT                        | O(log n)    | BST index update                    |
| SELECT * (full scan)          | O(n)        | Sequential row scan                 |
| SELECT WHERE first_col = val  | O(log n)    | BST index lookup                    |
| SELECT WHERE other_col op val | O(n)        | Full scan                           |
| INNER JOIN                    | O(n + m)    | Hash-join, average case             |
| Repeated SELECT (cache hit)   | O(1)        | LRU cache                           |

For the 10-million-row benchmark, the primary bottleneck is network round-trip
time for inserts. The engine itself processes individual inserts in microseconds.
Batch inserts (multiple VALUES in a single statement) are not implemented but
would significantly improve throughput by reducing round-trips.
