# FlexQL

A simplified SQL-like in-memory database driver implemented in C++17.

## Features

| Feature | Details |
|---|---|
| Types | `INT`, `DECIMAL`, `VARCHAR`, `DATETIME` |
| Commands | `CREATE TABLE`, `INSERT INTO`, `SELECT`, `WHERE`, `INNER JOIN` |
| WHERE operators | `=` `!=` `<` `>` `<=` `>=` |
| Row expiry | `INSERT ... TTL <seconds>` |
| Indexing | BST primary index on first column — O(log n) equality lookup |
| Caching | LRU cache (1024 entries) — identical SELECTs served instantly |
| Concurrency | One thread per client, mutex-protected engine |

---

## Quick Start

### 1. Build

```bash
make
```

Produces `bin/flexql-server` and `bin/flexql-client`.

### 2. Start the server

```bash
./bin/flexql-server 9000
```

### 3. Connect a client (new terminal)

```bash
./bin/flexql-client 127.0.0.1 9000
```

### 4. Run SQL

```sql
CREATE TABLE STUDENT(ID INT, NAME VARCHAR);
INSERT INTO STUDENT VALUES (1,'Alice');
INSERT INTO STUDENT VALUES (2,'Bob');
SELECT * FROM STUDENT;
SELECT * FROM STUDENT WHERE ID > 1;
.exit
```

---

## Supported SQL Syntax

### CREATE TABLE
```sql
CREATE TABLE table_name (col1 TYPE, col2 TYPE, ...);
```
Supported types: `INT`, `DECIMAL`, `VARCHAR`, `DATETIME`

### INSERT
```sql
INSERT INTO table_name VALUES (val1, val2, ...);
INSERT INTO table_name VALUES (val1, val2, ...) TTL 3600;  -- expires in 1 hour
```

### SELECT
```sql
SELECT * FROM table_name;
SELECT col1, col2 FROM table_name;
SELECT * FROM table_name WHERE col = value;
SELECT * FROM t1 INNER JOIN t2 ON t1.col = t2.col;
SELECT * FROM t1 INNER JOIN t2 ON t1.col = t2.col WHERE t2.col2 = 'x';
```

### DATETIME values
```sql
INSERT INTO events VALUES (1, 'Launch', '2024-03-15 09:00:00');
SELECT * FROM events WHERE ts > '2024-01-01';
```

---

## Running the Tests

```bash
make test
```

Starts a temporary server on port 9099, runs 42 automated tests, then stops the server.

Expected output:
```
Results: 42 passed, 0 failed
```

---

## Performance Benchmark

```bash
./scripts/benchmark.sh 127.0.0.1 9000 1000000
```

Measures INSERT throughput, full scan, index lookup, range scan, DATETIME
range, VARCHAR scan, and LRU cache hit time. Reports peak server RSS memory.

Change the third argument to `10000000` for a 10-million-row benchmark.

---

## C API

```c
#include "include/flexql.h"

// Callback: invoked once per result row
int my_callback(void *data, int nCols, char **values, char **colNames) {
    for (int i = 0; i < nCols; i++)
        printf("%s = %s\n", colNames[i], values[i]);
    printf("\n");
    return 0;  // return 1 to abort early
}

int main() {
    FlexQL *db;
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) return 1;

    char *err = NULL;
    flexql_exec(db, "CREATE TABLE T(ID INT, NAME VARCHAR);", NULL, NULL, &err);
    flexql_exec(db, "INSERT INTO T VALUES (1,'Alice');",    NULL, NULL, &err);
    flexql_exec(db, "SELECT * FROM T;", my_callback, NULL, &err);

    if (err) { fprintf(stderr, "%s\n", err); flexql_free(err); }
    flexql_close(db);
}
```

Compile against the API:
```bash
g++ -std=c++17 -I include -o myapp myapp.cpp src/client/flexql_api.cpp
```

---

## Project Structure

```
flexql/
├── Makefile
├── README.md
├── DESIGN.md               — design decisions, algorithms, complexity table
├── include/
│   ├── flexql.h            — public C API
│   ├── common/types.h      — ColType, CellValue, Row, Table, Database
│   ├── parser/lexer.h      — token types
│   ├── parser/parser.h     — TokenStream, Condition, JoinCondition
│   ├── query/engine.h      — Engine class
│   ├── query/result.h      — QueryResult struct
│   ├── cache/lru_cache.h   — LRUCache
│   ├── network/protocol.h  — wire protocol constants
│   ├── expiration/ttl.h    — isExpired()
│   ├──storage/storage.h     Header file storae
│   └── concurrency/        — MutexGuard RAII wrapper
├── src/
│   ├── parser/lexer.cpp
│   ├── parser/parser.cpp
│   ├── cache/lru_cache.cpp
│   ├── query/engine.cpp    — CREATE / INSERT / SELECT / INNER JOIN
│   ├── storage/storage.cpp   storage implementation
│   ├── server/server.cpp   — TCP server only
│   └── client/
│       ├── flexql_api.cpp  — C API implementation (no main)
│       └── client.cpp      — interactive REPL main
├── tests/
│   └── test_all.cpp        — 42 automated tests
└── scripts/
    └── benchmark.sh        — performance benchmark
```
