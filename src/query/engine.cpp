/*
 * src/query/engine.cpp
 * ====================
 * SQL Engine — CREATE, INSERT, SELECT, real INNER JOIN (hash-join).
 *
 * Storage:   row-major (vector<Row> per Table)
 * Indexing:  BST (std::map) on first column — O(log n) equality lookup
 * Cache:     LRU cache checked BEFORE every SELECT
 * Expiry:    rows past their TTL are silently skipped on read
 */
#include "../../include/query/engine.h"
#include "../../include/expiration/ttl.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <ctime>
#include <iostream>

// ── Helpers ───────────────────────────────────────────────────────────────────

static int64_t parseDT(const std::string& s) {
    struct tm tm_s = {};
    const char* ok = strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm_s);
    if (!ok) ok = strptime(s.c_str(), "%Y-%m-%d", &tm_s);
    if (!ok) return 0;
    tm_s.tm_isdst = -1;
    return (int64_t)mktime(&tm_s);
}

// Convert a CellValue to its display string.
// ColType needed so DATETIME (int64_t) renders as ISO date.
static std::string cellStr(const CellValue& cv, ColType ct) {
    if (std::holds_alternative<int64_t>(cv)) {
        int64_t v = std::get<int64_t>(cv);
        if (ct == ColType::DATETIME) {
            time_t t = (time_t)v;
            struct tm tm_s = {};
            localtime_r(&t, &tm_s);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_s);
            return std::string(buf);
        }
        return std::to_string(v);
    }
    if (std::holds_alternative<double>(cv)) {
        double d = std::get<double>(cv);
        if (d == (int64_t)d) return std::to_string((int64_t)d);
        std::ostringstream oss; oss << d; return oss.str();
    }
    return std::get<std::string>(cv);
}

static std::string sanitize(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out += (c == '\t') ? ' ' : c;
    return out;
}

// Compare a cell value against a condition value
static bool evalCond(const CellValue& cell, const std::string& op,
                     const CellValue& cond, ColType ct)
{
    bool cellN = std::holds_alternative<int64_t>(cell) || std::holds_alternative<double>(cell);
    bool condN = std::holds_alternative<int64_t>(cond) || std::holds_alternative<double>(cond);

    if (cellN && condN) {
        double a = std::holds_alternative<int64_t>(cell)
                   ? (double)std::get<int64_t>(cell) : std::get<double>(cell);
        double b = std::holds_alternative<int64_t>(cond)
                   ? (double)std::get<int64_t>(cond) : std::get<double>(cond);
        if (op=="=")  return a==b;  if (op=="!=") return a!=b;
        if (op=="<")  return a<b;   if (op==">")  return a>b;
        if (op=="<=") return a<=b;  if (op==">=") return a>=b;
    }
    if (ct == ColType::DATETIME && std::holds_alternative<int64_t>(cell)) {
        int64_t a = std::get<int64_t>(cell), b = 0;
        if (std::holds_alternative<int64_t>(cond))      b = std::get<int64_t>(cond);
        else if (std::holds_alternative<std::string>(cond)) b = parseDT(std::get<std::string>(cond));
        if (op=="=")  return a==b;  if (op=="!=") return a!=b;
        if (op=="<")  return a<b;   if (op==">")  return a>b;
        if (op=="<=") return a<=b;  if (op==">=") return a>=b;
    }
    std::string a = cellStr(cell, ct), b = cellStr(cond, ColType::VARCHAR);
    if (op=="=")  return a==b;  if (op=="!=") return a!=b;
    if (op=="<")  return a<b;   if (op==">")  return a>b;
    if (op=="<=") return a<=b;  if (op==">=") return a>=b;
    return false;
}

// ── Type parsing ──────────────────────────────────────────────────────────────
ColType Engine::parseColType(TokenStream& ts) {
    std::string t = ts.expectName();
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    ColType ct;
    if      (t=="INT"||t=="INTEGER")                              ct = ColType::INT;
    else if (t=="DECIMAL"||t=="FLOAT"||t=="DOUBLE"||t=="NUMERIC") ct = ColType::DECIMAL;
    else if (t=="VARCHAR"||t=="TEXT"||t=="CHAR")                  ct = ColType::VARCHAR;
    else if (t=="DATETIME"||t=="DATE"||t=="TIME")                 ct = ColType::DATETIME;
    else throw std::runtime_error(
        "Unsupported type '" + t + "'. Use INT, DECIMAL, VARCHAR, or DATETIME.");
    // Consume optional size parameter: VARCHAR(64), DECIMAL(10,2) etc.
    if (ts.check(TT::LPAREN)) {
        ts.consume();                                        // consume (
        while (!ts.atEnd() && !ts.check(TT::RPAREN))
            ts.consume();                                    // consume 64 / 10,2 etc.
        if (ts.check(TT::RPAREN)) ts.consume();             // consume )
    }
    // Consume optional modifiers: PRIMARY KEY NOT NULL
    while (ts.peek().type == TT::KEYWORD) {
        const std::string& kw = ts.peek().value;
        if (kw=="PRIMARY"||kw=="KEY"||kw=="NOT"||kw=="NULL") ts.consume(); else break;
    }
    return ct;
}

// ── Cell builder ──────────────────────────────────────────────────────────────
CellValue Engine::buildCell(const Token& tok, ColType ct, const std::string& col) {
    switch (ct) {
    case ColType::INT:
        if (tok.type==TT::NUMBER||tok.type==TT::STRING_LIT)
            return (int64_t)std::stoll(tok.value);
        throw std::runtime_error("INT expected for '"+col+"', got '"+tok.value+"'");
    case ColType::DECIMAL:
        if (tok.type==TT::NUMBER) return std::stod(tok.value);
        throw std::runtime_error("DECIMAL expected for '"+col+"', got '"+tok.value+"'");
    case ColType::VARCHAR:
        if (tok.type==TT::STRING_LIT||tok.type==TT::IDENT||tok.type==TT::NUMBER)
            return tok.value;
        throw std::runtime_error("VARCHAR expected for '"+col+"', got '"+tok.value+"'");
    case ColType::DATETIME:
        if (tok.type==TT::DATETIME_LIT||tok.type==TT::STRING_LIT) {
            int64_t v = parseDT(tok.value);
            if (v==0) throw std::runtime_error(
                "Cannot parse DATETIME '"+tok.value+"'. Use 'YYYY-MM-DD HH:MM:SS'.");
            return v;
        }
        if (tok.type==TT::NUMBER) return (int64_t)std::stoll(tok.value);
        throw std::runtime_error("DATETIME expected for '"+col+"', got '"+tok.value+"'");
    }
    throw std::runtime_error("Unknown type");
}

// ── Constructor: open storage and recover ─────────────────────────────────────
Engine::Engine() {
    std::string dataDir = "data/tables";
    std::string walPath = "data/wal/flexql.wal";

    // Ensure directories exist
    system("mkdir -p data/tables data/wal");

    if (!storage_.open(dataDir, walPath)) {
        std::cerr << "[engine] WARNING: Could not open storage. Running without persistence.\n";
        return;
    }
    if (!storage_.recover(db_)) {
        std::cerr << "[engine] WARNING: Recovery failed. Starting with empty database.\n";
    }
    std::cerr << "[engine] Ready. " << db_.tables.size() << " table(s) loaded from storage.\n";
}

void Engine::forceCheckpoint() {
    storage_.checkpoint(db_);
}

void Engine::maybeCheckpoint() {
    if (++opCount_ % CHECKPOINT_INTERVAL == 0)
        storage_.checkpoint(db_);
}

// ── execute() ─────────────────────────────────────────────────────────────────
QueryResult Engine::execute(const std::string& rawQuery) {
    QueryResult qr;
    std::string q = rawQuery;
    while (!q.empty() && std::isspace((unsigned char)q.front())) q.erase(q.begin());
    while (!q.empty() && std::isspace((unsigned char)q.back()))  q.pop_back();
    if (q.empty()) return qr;

    std::string lo = q;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    if (lo==".exit"||lo==".quit"||lo=="exit"||lo=="quit") {
        qr.message = "__CLOSE__"; return qr;
    }

    auto tokens = tokenize(q);
    TokenStream ts{tokens, 0};
    if (ts.atEnd()) return qr;
    if (ts.peek().type != TT::KEYWORD) {
        qr.ok=false; qr.message="Expected SQL keyword, got '"+ts.peek().value+"'"; return qr;
    }

    // LRU cache: serve identical SELECT instantly.
    // Skip cache for queries on tables that have TTL rows, because a cached
    // result may contain rows that have since expired.
    if (ts.peek().value == "SELECT") {
        // Quick scan to find the FROM table name and check hasTTLRows
        bool skipCache = false;
        for (std::size_t pi = 0; pi < tokens.size(); ++pi) {
            if (tokens[pi].type == TT::KEYWORD && tokens[pi].value == "FROM" &&
                pi+1 < tokens.size()) {
                const std::string& tname = tokens[pi+1].value;
                auto tit = db_.tables.find(tname);
                if (tit != db_.tables.end() && tit->second.hasTTLRows)
                    skipCache = true;
                break;
            }
        }
        if (!skipCache) {
            std::vector<std::string> cc; std::vector<std::vector<std::string>> cr;
            if (cache_.get(rawQuery, cc, cr)) {
                qr.isSelect=true; qr.colNames=cc; qr.rows=cr; return qr;
            }
        }
    }

    try {
        const std::string& kw = ts.peek().value;
        if      (kw=="CREATE") doCreate(ts, qr);
        else if (kw=="INSERT") doInsert(ts, qr);
        else if (kw=="SELECT") doSelect(ts, qr);
        else if (kw=="DELETE") doDelete(ts, qr);
        else { qr.ok=false; qr.message="Unknown command: "+kw; }
    } catch (const std::exception& e) {
        qr.ok=false; qr.message=e.what();
    }

    // Only cache SELECT results for tables without TTL rows
    if (qr.ok && qr.isSelect) {
        bool hasTTL = false;
        for (std::size_t pi = 0; pi < tokens.size(); ++pi) {
            if (tokens[pi].type == TT::KEYWORD && tokens[pi].value == "FROM" &&
                pi+1 < tokens.size()) {
                auto tit = db_.tables.find(tokens[pi+1].value);
                if (tit != db_.tables.end() && tit->second.hasTTLRows)
                    hasTTL = true;
                break;
            }
        }
        if (!hasTTL) cache_.put(rawQuery, qr.colNames, qr.rows);
    }
    return qr;
}

// ── CREATE TABLE ──────────────────────────────────────────────────────────────
void Engine::doCreate(TokenStream& ts, QueryResult& qr) {
    ts.expectKeyword("CREATE"); ts.expectKeyword("TABLE");
    // Support: CREATE TABLE IF NOT EXISTS name (...)
    bool ifNotExists = false;
    if (ts.peek().type == TT::KEYWORD && ts.peek().value == "IF") {
        ts.consume();
        ts.expectKeyword("NOT");
        ts.expectKeyword("EXISTS");
        ifNotExists = true;
    }
    std::string tName = ts.expectName();
    if (db_.tables.count(tName)) {
        if (ifNotExists) { qr.message="Table '"+tName+"' already exists (skipped)."; return; }
        qr.ok=false; qr.message="Table '"+tName+"' already exists."; return;
    }
    ts.expect(TT::LPAREN, "(");
    Table tbl; tbl.name = tName;
    while (true) {
        Column col; col.name=ts.expectName(); col.type=parseColType(ts);
        tbl.columns.push_back(col);
        if (ts.check(TT::COMMA)) { ts.consume(); continue; } break;
    }
    ts.expect(TT::RPAREN, ")");
    if (ts.check(TT::SEMICOLON)) ts.consume();
    db_.tables[tName] = std::move(tbl);
    // Persist: write CREATE_TABLE to WAL before returning success
    storage_.walCreateTable(db_.tables[tName]);
    cache_.invalidateTable(tName);
    maybeCheckpoint();
    qr.message = "Table '"+tName+"' created successfully.";
}

// ── INSERT ────────────────────────────────────────────────────────────────────
void Engine::doInsert(TokenStream& ts, QueryResult& qr) {
    ts.expectKeyword("INSERT"); ts.expectKeyword("INTO");
    std::string tName = ts.expectName();
    if (!db_.tables.count(tName)) {
        qr.ok=false; qr.message="Table '"+tName+"' does not exist."; return;
    }
    Table& tbl = db_.tables[tName];
    ts.expectKeyword("VALUES"); ts.expect(TT::LPAREN, "(");
    Row row;
    for (std::size_t c=0; c<tbl.columns.size(); ++c) {
        if (c>0) ts.expect(TT::COMMA, ",");
        Token v = ts.consume();
        try { row.cells.push_back(buildCell(v, tbl.columns[c].type, tbl.columns[c].name)); }
        catch (const std::exception& e) { qr.ok=false; qr.message=e.what(); return; }
    }
    ts.expect(TT::RPAREN, ")");
    if (ts.checkKeyword("TTL")) {
        ts.consume();
        Token t = ts.expect(TT::NUMBER, "TTL seconds");
        long long ttl = (long long)std::stod(t.value);
        if (ttl<=0) { qr.ok=false; qr.message="TTL must be positive."; return; }
        row.expires = time(nullptr)+ttl;
        tbl.hasTTLRows = true;
    }
    // Collect all rows first (WAL-then-memory for atomicity)
    std::vector<Row> rowsBatch;
    rowsBatch.push_back(std::move(row));

    // Multi-row INSERT: VALUES (r1),(r2),(r3),...
    while (ts.check(TT::COMMA)) {
        ts.consume();
        if (!ts.check(TT::LPAREN)) break;
        ts.consume();
        Row rowN;
        for (std::size_t c=0; c<tbl.columns.size(); ++c) {
            if (c>0) ts.expect(TT::COMMA, ",");
            Token v = ts.consume();
            try { rowN.cells.push_back(buildCell(v, tbl.columns[c].type, tbl.columns[c].name)); }
            catch (const std::exception& e) { qr.ok=false; qr.message=e.what(); return; }
        }
        ts.expect(TT::RPAREN, ")");
        rowsBatch.push_back(std::move(rowN));
    }

    if (ts.check(TT::SEMICOLON)) ts.consume();

    // Build column type list for WAL serialisation
    std::vector<ColType> colTypes;
    for (auto& col : tbl.columns) colTypes.push_back(col.type);

    // Write entire batch as ONE WAL record (single fsync) for performance,
    // then update memory. This is the key optimisation for batch inserts.
    storage_.walInsertBatch(tName, rowsBatch, colTypes);
    for (auto& r : rowsBatch) {
        tbl.primaryIndex[cellStr(r.cells[0], tbl.columns[0].type)].push_back(tbl.rows.size());
        tbl.rows.push_back(std::move(r));
    }

    cache_.invalidateTable(tName);
    maybeCheckpoint();
    qr.message = std::to_string(rowsBatch.size()) + " row(s) inserted into '"+tName+"'.";
}

// ── SELECT ────────────────────────────────────────────────────────────────────
void Engine::doSelect(TokenStream& ts, QueryResult& qr) {
    ts.expectKeyword("SELECT"); qr.isSelect = true;
    bool selectAll=false; std::vector<std::string> reqCols;
    if (ts.check(TT::STAR)) { ts.consume(); selectAll=true; }
    else {
        while (true) {
            std::string nm=ts.expectName();
            if (ts.check(TT::DOT)) { ts.consume(); nm+="."+ts.expectName(); }
            reqCols.push_back(nm);
            if (ts.check(TT::COMMA)) { ts.consume(); continue; } break;
        }
    }
    ts.expectKeyword("FROM");
    std::string leftName = ts.expectName();
    if (!db_.tables.count(leftName)) {
        qr.ok=false; qr.message="Table '"+leftName+"' does not exist."; return;
    }
    if (ts.checkKeyword("INNER")) { doInnerJoin(ts,leftName,selectAll,reqCols,qr); return; }

    bool hasWhere=false; Condition cond;
    if (ts.checkKeyword("WHERE")) { ts.consume(); cond=parseCondition(ts); hasWhere=true; }
    // Consume ORDER BY clause (basic - just parse and ignore for compatibility)
    std::string orderByCol; bool orderAsc = true;
    if (ts.checkKeyword("ORDER")) {
        ts.consume(); ts.expectKeyword("BY");
        orderByCol = ts.expectName();
        if (ts.check(TT::DOT)) { ts.consume(); orderByCol = ts.expectName(); }
        if (ts.checkKeyword("DESC")) { ts.consume(); orderAsc = false; }
        else if (ts.checkKeyword("ASC")) { ts.consume(); }
    }
    if (ts.check(TT::SEMICOLON)) ts.consume();
    Table& tbl = db_.tables[leftName];

    std::vector<int> outIdx;
    if (selectAll) { for (int i=0;i<(int)tbl.columns.size();++i) outIdx.push_back(i); }
    else {
        for (const auto& sc : reqCols) {
            std::string cn=sc; auto dot=sc.find('.');
            if (dot!=std::string::npos) cn=sc.substr(dot+1);
            int ci=tbl.colIndex(cn);
            if (ci==-1) { qr.ok=false; qr.message="Column '"+cn+"' not found in '"+leftName+"'."; return; }
            outIdx.push_back(ci);
        }
    }
    for (int ci : outIdx) qr.colNames.push_back(tbl.columns[ci].name);

    // Primary index: WHERE on first col with = -> O(log n)
    std::vector<std::size_t> toScan; bool indexed=false;
    if (hasWhere) {
        std::string cc=cond.colRef; auto dot=cc.find('.');
        if (dot!=std::string::npos) cc=cc.substr(dot+1);
        if (cc==tbl.columns[0].name && cond.op=="=") {
            std::string key=cellStr(cond.value, tbl.columns[0].type);
            auto it=tbl.primaryIndex.find(key);
            if (it==tbl.primaryIndex.end()) return;
            toScan=it->second; indexed=true;
        }
    }
    if (!indexed) { toScan.resize(tbl.rows.size()); std::iota(toScan.begin(),toScan.end(),0); }

    for (std::size_t ri : toScan) {
        const Row& row=tbl.rows[ri];
        if (isExpired(row)) continue;
        if (hasWhere) {
            std::string cc=cond.colRef; auto dot=cc.find('.');
            if (dot!=std::string::npos) cc=cc.substr(dot+1);
            int ci=tbl.colIndex(cc);
            if (ci==-1) { qr.ok=false; qr.message="Column '"+cc+"' not found."; return; }
            if (!evalCond(row.cells[ci],cond.op,cond.value,tbl.columns[ci].type)) continue;
        }
        std::vector<std::string> rowData;
        for (int ci : outIdx)
            rowData.push_back(sanitize(cellStr(row.cells[ci], tbl.columns[ci].type)));
        qr.rows.push_back(std::move(rowData));
    }

    // Apply ORDER BY sort if specified
    if (!orderByCol.empty() && !qr.colNames.empty()) {
        int sortColIdx = -1;
        for (int i=0; i<(int)qr.colNames.size(); ++i)
            if (qr.colNames[i] == orderByCol) { sortColIdx = i; break; }
        if (sortColIdx >= 0) {
            std::stable_sort(qr.rows.begin(), qr.rows.end(),
                [&](const std::vector<std::string>& a, const std::vector<std::string>& b){
                    return orderAsc ? (a[sortColIdx] < b[sortColIdx])
                                    : (a[sortColIdx] > b[sortColIdx]);
                });
        }
    }
}


// ── DELETE FROM table ─────────────────────────────────────────────────────────
void Engine::doDelete(TokenStream& ts, QueryResult& qr) {
    ts.expectKeyword("DELETE");
    ts.expectKeyword("FROM");
    std::string tName = ts.expectName();
    if (!db_.tables.count(tName)) {
        qr.ok=false; qr.message="Table '"+tName+"' does not exist."; return;
    }
    // Optional WHERE (delete matching rows only); no WHERE = delete all
    bool hasWhere=false; Condition cond;
    if (ts.checkKeyword("WHERE")) { ts.consume(); cond=parseCondition(ts); hasWhere=true; }
    if (ts.check(TT::SEMICOLON)) ts.consume();

    Table& tbl = db_.tables[tName];
    if (!hasWhere) {
        long long deleted = (long long)tbl.rows.size();
        storage_.walDeleteAll(tName);  // WAL before memory
        tbl.rows.clear();
        tbl.primaryIndex.clear();
        cache_.invalidateTable(tName);
        maybeCheckpoint();
        qr.message = std::to_string(deleted) + " row(s) deleted from '"+tName+"'.";
        return;
    }
    // WHERE delete: filter rows
    std::vector<Row> remaining;
    long long deleted = 0;
    for (auto& row : tbl.rows) {
        std::string cc=cond.colRef; auto dot=cc.find('.');
        if (dot!=std::string::npos) cc=cc.substr(dot+1);
        int ci=tbl.colIndex(cc);
        if (ci==-1) { qr.ok=false; qr.message="Column '"+cc+"' not found."; return; }
        if (evalCond(row.cells[ci], cond.op, cond.value, tbl.columns[ci].type))
            ++deleted;
        else
            remaining.push_back(std::move(row));
    }
    tbl.rows = std::move(remaining);
    // Rebuild primary index after deletion
    tbl.primaryIndex.clear();
    for (std::size_t i=0; i<tbl.rows.size(); ++i)
        tbl.primaryIndex[cellStr(tbl.rows[i].cells[0], tbl.columns[0].type)].push_back(i);
    cache_.invalidateTable(tName);
    qr.message = std::to_string(deleted) + " row(s) deleted from '"+tName+"'.";
}

// ── INNER JOIN  (hash-join equi-join — NOT cross join) ────────────────────────
/*
 * Only rows where leftTable.joinCol = rightTable.joinCol are emitted.
 * Rows with no match on either side are excluded (true INNER JOIN).
 *
 * Algorithm — two-phase hash join:
 *   Phase 1: hash the right table's join-column values -> row indices.
 *   Phase 2: scan the left table; probe the hash map for each row.
 * Time: O(n + m) average.
 */
void Engine::doInnerJoin(TokenStream& ts, const std::string& leftName,
                         bool selectAll, const std::vector<std::string>& reqCols,
                         QueryResult& qr)
{
    ts.expectKeyword("INNER"); ts.expectKeyword("JOIN");
    std::string rightName = ts.expectName();
    if (!db_.tables.count(rightName)) {
        qr.ok=false; qr.message="Table '"+rightName+"' does not exist."; return;
    }
    ts.expectKeyword("ON");
    JoinCondition jc = parseJoinCondition(ts);

    // Accept ON clause in either column order
    if (jc.leftTable==rightName && jc.rightTable==leftName) {
        std::swap(jc.leftTable, jc.rightTable);
        std::swap(jc.leftCol,   jc.rightCol);
    }
    if (jc.leftTable!=leftName || jc.rightTable!=rightName) {
        qr.ok=false;
        qr.message="ON tables '"+jc.leftTable+"'/'"+jc.rightTable+
                   "' do not match FROM tables '"+leftName+"'/'"+rightName+"'.";
        return;
    }

    bool hasWhere=false; Condition wCond;
    if (ts.checkKeyword("WHERE")) { ts.consume(); wCond=parseCondition(ts); hasWhere=true; }
    if (ts.check(TT::SEMICOLON)) ts.consume();

    Table& L = db_.tables[leftName];
    Table& R = db_.tables[rightName];
    int lJoinIdx=L.colIndex(jc.leftCol), rJoinIdx=R.colIndex(jc.rightCol);
    if (lJoinIdx==-1) { qr.ok=false; qr.message="Join col '"+jc.leftCol+"' not found in '"+leftName+"'."; return; }
    if (rJoinIdx==-1) { qr.ok=false; qr.message="Join col '"+jc.rightCol+"' not found in '"+rightName+"'."; return; }

    // Build output column list: {0=left|1=right, col index}
    std::vector<std::pair<int,int>> outCols;
    if (selectAll) {
        for (int i=0;i<(int)L.columns.size();++i) { outCols.push_back({0,i}); qr.colNames.push_back(L.name+"."+L.columns[i].name); }
        for (int i=0;i<(int)R.columns.size();++i) { outCols.push_back({1,i}); qr.colNames.push_back(R.name+"."+R.columns[i].name); }
    } else {
        for (const auto& sc : reqCols) {
            std::string tname,cname; auto dot=sc.find('.');
            if (dot!=std::string::npos){tname=sc.substr(0,dot);cname=sc.substr(dot+1);}else cname=sc;
            bool found=false;
            if (tname.empty()||tname==leftName) { int ci=L.colIndex(cname); if(ci!=-1){outCols.push_back({0,ci});qr.colNames.push_back(L.name+"."+cname);found=true;} }
            if (!found&&(tname.empty()||tname==rightName)) { int ci=R.colIndex(cname); if(ci!=-1){outCols.push_back({1,ci});qr.colNames.push_back(R.name+"."+cname);found=true;} }
            if (!found) { qr.ok=false; qr.message="Column '"+sc+"' not found in either table."; return; }
        }
    }

    // Phase 1: build probe hash table from R
    std::unordered_map<std::string,std::vector<std::size_t>> probe;
    for (std::size_t ri=0;ri<R.rows.size();++ri) {
        if (isExpired(R.rows[ri])) continue;
        probe[cellStr(R.rows[ri].cells[rJoinIdx], R.columns[rJoinIdx].type)].push_back(ri);
    }

    // Phase 2: scan L, probe into R
    for (std::size_t li=0;li<L.rows.size();++li) {
        const Row& lRow=L.rows[li];
        if (isExpired(lRow)) continue;
        auto it=probe.find(cellStr(lRow.cells[lJoinIdx], L.columns[lJoinIdx].type));
        if (it==probe.end()) continue;  // no match -> excluded from INNER JOIN

        for (std::size_t ri : it->second) {
            const Row& rRow=R.rows[ri];
            if (hasWhere) {
                std::string cc=wCond.colRef; std::string tname,cname; auto dot=cc.find('.');
                if (dot!=std::string::npos){tname=cc.substr(0,dot);cname=cc.substr(dot+1);}else cname=cc;
                const CellValue* cv=nullptr; ColType cct=ColType::VARCHAR;
                if (tname.empty()||tname==leftName)  { int ci=L.colIndex(cname); if(ci!=-1){cv=&lRow.cells[ci];cct=L.columns[ci].type;} }
                if (!cv&&(tname.empty()||tname==rightName)) { int ci=R.colIndex(cname); if(ci!=-1){cv=&rRow.cells[ci];cct=R.columns[ci].type;} }
                if (!cv) { qr.ok=false; qr.message="WHERE col '"+wCond.colRef+"' not found."; return; }
                if (!evalCond(*cv,wCond.op,wCond.value,cct)) continue;
            }
            std::vector<std::string> rowData;
            for (auto [ti,ci] : outCols) {
                const Row& r=(ti==0)?lRow:rRow; const Table& T=(ti==0)?L:R;
                rowData.push_back(sanitize(cellStr(r.cells[ci], T.columns[ci].type)));
            }
            qr.rows.push_back(std::move(rowData));
        }
    }
}
