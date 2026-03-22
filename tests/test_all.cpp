/*
 * tests/test_all.cpp
 * ==================
 * Automated test suite for FlexQL.
 * Uses the public C API (flexql.h) to exercise every feature.
 *
 * Compile:
 *   g++ -std=c++17 -O2 -I ../include -o test_all test_all.cpp
 *
 * Run (server must be running on port 9000):
 *   ./test_all 127.0.0.1 9000
 *
 * Or use the convenience target:
 *   make test
 */
#include "../include/flexql.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

// ── Tiny test framework ───────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

#define EXPECT_TRUE(cond, msg) \
    do { if (cond) { printf("  PASS  %s\n", msg); ++g_pass; } \
         else      { printf("  FAIL  %s\n", msg); ++g_fail; } } while(0)

#define EXPECT_EQ(a, b, msg) EXPECT_TRUE((a)==(b), msg)

// ── Collected-rows callback ───────────────────────────────────────────────────
struct RowStore {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string>              colNames;
    bool                                  headersSet = false;
};

static int collectCb(void* data, int n, char** vals, char** cols) {
    RowStore* rs = static_cast<RowStore*>(data);
    if (!rs->headersSet) {
        for (int i = 0; i < n; ++i) rs->colNames.push_back(cols[i]);
        rs->headersSet = true;
    }
    std::vector<std::string> row;
    for (int i = 0; i < n; ++i) row.push_back(vals[i]);
    rs->rows.push_back(row);
    return 0;
}

// ── Helper: run SQL and return FLEXQL_OK / FLEXQL_ERROR ──────────────────────
static int run(FlexQL* db, const char* sql) {
    char* err = nullptr;
    int rc = flexql_exec(db, sql, nullptr, nullptr, &err);
    if (rc != FLEXQL_OK) {
        printf("  [SQL ERR] %s\n  -> %s\n", sql, err ? err : "(null)");
        flexql_free(err);
    }
    return rc;
}

static RowStore query(FlexQL* db, const char* sql) {
    RowStore rs;
    char* err = nullptr;
    flexql_exec(db, sql, collectCb, &rs, &err);
    flexql_free(err);
    return rs;
}

// ============================================================
//  TEST GROUPS
// ============================================================

void test_create_and_insert(FlexQL* db) {
    printf("\n--- CREATE TABLE & INSERT ---\n");
    EXPECT_EQ(run(db, "CREATE TABLE STUDENT(ID INT, NAME VARCHAR);"),
              FLEXQL_OK, "CREATE TABLE with INT and VARCHAR");

    EXPECT_EQ(run(db, "INSERT INTO STUDENT VALUES (1,'Alice');"),
              FLEXQL_OK, "INSERT row 1");
    EXPECT_EQ(run(db, "INSERT INTO STUDENT VALUES (2,'Bob');"),
              FLEXQL_OK, "INSERT row 2");
    EXPECT_EQ(run(db, "INSERT INTO STUDENT VALUES (3,'Carol');"),
              FLEXQL_OK, "INSERT row 3");

    // Duplicate table should fail
    EXPECT_EQ(run(db, "CREATE TABLE STUDENT(X INT);"),
              FLEXQL_ERROR, "Duplicate CREATE TABLE returns error");
}

void test_select_all(FlexQL* db) {
    printf("\n--- SELECT * ---\n");
    auto rs = query(db, "SELECT * FROM STUDENT;");
    EXPECT_EQ((int)rs.rows.size(), 3, "SELECT * returns 3 rows");
    EXPECT_EQ(rs.rows[0][0], std::string("1"),     "Row 0 ID = 1");
    EXPECT_EQ(rs.rows[0][1], std::string("Alice"),  "Row 0 NAME = Alice");
    EXPECT_EQ(rs.rows[2][1], std::string("Carol"),  "Row 2 NAME = Carol");
}

void test_select_columns(FlexQL* db) {
    printf("\n--- SELECT specific columns ---\n");
    auto rs = query(db, "SELECT NAME FROM STUDENT;");
    EXPECT_EQ((int)rs.rows.size(), 3, "SELECT NAME returns 3 rows");
    EXPECT_EQ((int)rs.colNames.size(), 1, "Only 1 column returned");
    EXPECT_EQ(rs.colNames[0], std::string("NAME"),  "Column name is NAME");
    EXPECT_EQ(rs.rows[1][0], std::string("Bob"),    "Row 1 NAME = Bob");
}

void test_where_clause(FlexQL* db) {
    printf("\n--- WHERE clause ---\n");
    auto rs = query(db, "SELECT * FROM STUDENT WHERE ID = 2;");
    EXPECT_EQ((int)rs.rows.size(), 1, "WHERE ID=2 returns 1 row");
    EXPECT_EQ(rs.rows[0][1], std::string("Bob"), "WHERE ID=2 returns Bob");

    rs = query(db, "SELECT * FROM STUDENT WHERE ID > 1;");
    EXPECT_EQ((int)rs.rows.size(), 2, "WHERE ID>1 returns 2 rows");

    rs = query(db, "SELECT * FROM STUDENT WHERE NAME = 'Alice';");
    EXPECT_EQ((int)rs.rows.size(), 1, "WHERE NAME='Alice' returns 1 row");

    rs = query(db, "SELECT * FROM STUDENT WHERE ID >= 2;");
    EXPECT_EQ((int)rs.rows.size(), 2, "WHERE ID>=2 returns 2 rows");

    rs = query(db, "SELECT * FROM STUDENT WHERE ID != 1;");
    EXPECT_EQ((int)rs.rows.size(), 2, "WHERE ID!=1 returns 2 rows");
}

void test_int_type(FlexQL* db) {
    printf("\n--- INT type ---\n");
    run(db, "CREATE TABLE NUMS(A INT, B INT);");
    run(db, "INSERT INTO NUMS VALUES (100, 200);");
    run(db, "INSERT INTO NUMS VALUES (-5, 999);");

    auto rs = query(db, "SELECT * FROM NUMS WHERE A = -5;");
    EXPECT_EQ((int)rs.rows.size(), 1, "Negative INT insert and select");
    EXPECT_EQ(rs.rows[0][1], std::string("999"), "Negative INT WHERE returns correct row");

    rs = query(db, "SELECT * FROM NUMS WHERE B > 500;");
    EXPECT_EQ((int)rs.rows.size(), 1, "INT comparison > 500");
}

void test_decimal_type(FlexQL* db) {
    printf("\n--- DECIMAL type ---\n");
    run(db, "CREATE TABLE PRICES(ITEM VARCHAR, COST DECIMAL);");
    run(db, "INSERT INTO PRICES VALUES ('Apple', 1.50);");
    run(db, "INSERT INTO PRICES VALUES ('Banana', 0.75);");
    run(db, "INSERT INTO PRICES VALUES ('Cherry', 3.25);");

    auto rs = query(db, "SELECT * FROM PRICES WHERE COST > 1;");
    EXPECT_EQ((int)rs.rows.size(), 2, "DECIMAL WHERE COST>1 returns 2 rows");

    rs = query(db, "SELECT * FROM PRICES WHERE COST = 0.75;");
    EXPECT_EQ((int)rs.rows.size(), 1, "DECIMAL exact match");
    EXPECT_EQ(rs.rows[0][0], std::string("Banana"), "DECIMAL exact match returns Banana");
}

void test_datetime_type(FlexQL* db) {
    printf("\n--- DATETIME type ---\n");
    run(db, "CREATE TABLE EVENTS(ID INT, NAME VARCHAR, TS DATETIME);");
    run(db, "INSERT INTO EVENTS VALUES (1,'Launch','2024-01-15 09:00:00');");
    run(db, "INSERT INTO EVENTS VALUES (2,'Review','2024-06-01 14:30:00');");
    run(db, "INSERT INTO EVENTS VALUES (3,'Release','2024-12-20 08:00:00');");

    auto rs = query(db, "SELECT * FROM EVENTS;");
    EXPECT_EQ((int)rs.rows.size(), 3, "DATETIME: SELECT * returns 3 rows");
    // Check it displays as a readable datetime string, not a raw number
    EXPECT_TRUE(rs.rows[0][2].find("2024") != std::string::npos,
                "DATETIME displays as readable date string");

    rs = query(db, "SELECT * FROM EVENTS WHERE TS > '2024-04-01';");
    EXPECT_EQ((int)rs.rows.size(), 2, "DATETIME WHERE TS > '2024-04-01' returns 2 rows");

    rs = query(db, "SELECT * FROM EVENTS WHERE TS = '2024-01-15 09:00:00';");
    EXPECT_EQ((int)rs.rows.size(), 1, "DATETIME exact match");
    EXPECT_EQ(rs.rows[0][1], std::string("Launch"), "DATETIME exact match returns Launch");
}

void test_inner_join(FlexQL* db) {
    printf("\n--- INNER JOIN (real equi-join) ---\n");
    run(db, "CREATE TABLE EMP(EID INT, ENAME VARCHAR, DID INT);");
    run(db, "CREATE TABLE DEPT(DID INT, DNAME VARCHAR);");
    run(db, "INSERT INTO EMP VALUES (1,'Alice',10);");
    run(db, "INSERT INTO EMP VALUES (2,'Bob',99);");   // DID 99 has no dept
    run(db, "INSERT INTO EMP VALUES (3,'Carol',20);");
    run(db, "INSERT INTO EMP VALUES (4,'Dave',10);");
    run(db, "INSERT INTO DEPT VALUES (10,'Engineering');");
    run(db, "INSERT INTO DEPT VALUES (20,'Sales');");

    auto rs = query(db, "SELECT * FROM EMP INNER JOIN DEPT ON EMP.DID = DEPT.DID;");
    // Alice(10), Carol(20), Dave(10) match; Bob(99) does NOT
    EXPECT_EQ((int)rs.rows.size(), 3, "INNER JOIN excludes unmatched rows (Bob)");

    // Verify Bob is absent
    bool bobFound = false;
    for (auto& row : rs.rows)
        if (row[1] == "Bob") bobFound = true;
    EXPECT_TRUE(!bobFound, "Bob (no matching dept) correctly excluded");

    // INNER JOIN with WHERE
    rs = query(db, "SELECT * FROM EMP INNER JOIN DEPT ON EMP.DID = DEPT.DID "
                   "WHERE DNAME = 'Engineering';");
    EXPECT_EQ((int)rs.rows.size(), 2, "INNER JOIN + WHERE returns Alice and Dave only");
}

void test_ttl(FlexQL* db) {
    printf("\n--- TTL expiration ---\n");
    run(db, "CREATE TABLE CACHE(K VARCHAR, V VARCHAR);");
    run(db, "INSERT INTO CACHE VALUES ('live','yes') TTL 100;");
    run(db, "INSERT INTO CACHE VALUES ('dead','yes') TTL 2;");

    // Check both rows are present immediately
    auto rs = query(db, "SELECT * FROM CACHE WHERE K = 'dead';");
    EXPECT_EQ((int)rs.rows.size(), 1, "Dead row present before TTL expires");
    rs = query(db, "SELECT * FROM CACHE WHERE K = 'live';");
    EXPECT_EQ((int)rs.rows.size(), 1, "Live row present before TTL expires");

    // Wait for the 2-second TTL to expire
    sleep(3);

    // Use a fresh unique query (bypasses LRU cache for the pre-expiry result)
    rs = query(db, "SELECT * FROM CACHE WHERE K = 'dead';");
    EXPECT_EQ((int)rs.rows.size(), 0, "Expired row gone after TTL");
    rs = query(db, "SELECT * FROM CACHE WHERE K = 'live';");
    EXPECT_EQ((int)rs.rows.size(), 1, "Non-expired row still present after TTL");
}

void test_lru_cache(FlexQL* db) {
    printf("\n--- LRU cache ---\n");
    // Run same SELECT twice — second should come from cache (same result)
    auto rs1 = query(db, "SELECT * FROM STUDENT;");
    auto rs2 = query(db, "SELECT * FROM STUDENT;");
    EXPECT_EQ(rs1.rows.size(), rs2.rows.size(), "Cached SELECT returns same row count");
    EXPECT_EQ(rs1.rows[0][0], rs2.rows[0][0],  "Cached SELECT returns same first value");
}

void test_error_handling(FlexQL* db) {
    printf("\n--- Error handling ---\n");
    EXPECT_EQ(run(db, "SELECT * FROM NONEXISTENT;"), FLEXQL_ERROR, "Unknown table -> error");
    EXPECT_EQ(run(db, "INSERT INTO NUMS VALUES (1);"), FLEXQL_ERROR, "Too few values -> error");
    EXPECT_EQ(run(db, "BLAH BLAH;"), FLEXQL_ERROR, "Unknown command -> error");
}

// ============================================================
//  MAIN
// ============================================================

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : "127.0.0.1";
    int         port = (argc >= 3) ? atoi(argv[2]) : 9000;

    printf("FlexQL Test Suite\n");
    printf("Connecting to %s:%d ...\n", host, port);

    FlexQL* db = nullptr;
    if (flexql_open(host, port, &db) != FLEXQL_OK) {
        fprintf(stderr, "Cannot connect to FlexQL server at %s:%d\n", host, port);
        fprintf(stderr, "Make sure the server is running first.\n");
        return 1;
    }
    printf("Connected.\n");

    test_create_and_insert(db);
    test_select_all(db);
    test_select_columns(db);
    test_where_clause(db);
    test_int_type(db);
    test_decimal_type(db);
    test_datetime_type(db);
    test_inner_join(db);
    test_ttl(db);
    test_lru_cache(db);
    test_error_handling(db);

    flexql_close(db);

    printf("\n══════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════\n");

    return (g_fail > 0) ? 1 : 0;
}
