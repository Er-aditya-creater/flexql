/*
 * src/client/flexql_api.cpp
 * =========================
 * FlexQL C API implementation.
 * No server code, no main() — links with client.cpp or test_all.cpp.
 *
 * Implements:
 *   flexql_open()  - TCP connect to server
 *   flexql_close() - graceful disconnect + free resources
 *   flexql_exec()  - send SQL, receive results, invoke callback
 *   flexql_free()  - free library-allocated memory
 */
#include "../../include/flexql.h"
#include "../../include/network/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

// ── Opaque FlexQL handle ──────────────────────────────────────────────────────
struct FlexQL {
    int    sockfd;
    char*  host;
    int    port;
    string recvBuf;   // persistent read-ahead buffer
};

// ── Socket helpers ────────────────────────────────────────────────────────────

static bool recvLine(FlexQL* db, string& line) {
    line.clear();
    while (true) {
        size_t pos = db->recvBuf.find('\n');
        if (pos != string::npos) {
            line = db->recvBuf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            db->recvBuf.erase(0, pos + 1);
            return true;
        }
        char buf[4096];
        int n = recv(db->sockfd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        db->recvBuf.append(buf, n);
    }
}

static bool sendAll(int fd, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(fd, data.c_str()+sent, data.size()-sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static vector<string> splitTab(const string& s) {
    vector<string> parts; string cur;
    for (char c : s) { if (c == '\t') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    return parts;
}

// ── flexql_open ───────────────────────────────────────────────────────────────
int flexql_open(const char* host, int port, FlexQL** db) {
    if (!db || !host) return FLEXQL_ERROR;
    const char* ip = (strcmp(host, "localhost") == 0) ? "127.0.0.1" : host;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return FLEXQL_ERROR;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) { close(sockfd); return FLEXQL_ERROR; }
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sockfd); return FLEXQL_ERROR; }

    FlexQL* h = new FlexQL();
    h->sockfd = sockfd;
    h->host   = strdup(host);
    h->port   = port;
    *db = h;
    return FLEXQL_OK;
}

// ── flexql_close ──────────────────────────────────────────────────────────────
int flexql_close(FlexQL* db) {
    if (!db) return FLEXQL_ERROR;
    sendAll(db->sockfd, string(Protocol::QUIT) + "\n");
    close(db->sockfd);
    free(db->host);
    delete db;
    return FLEXQL_OK;
}

// ── flexql_exec ───────────────────────────────────────────────────────────────
int flexql_exec(FlexQL* db, const char* sql,
                int (*callback)(void*, int, char**, char**),
                void* arg, char** errmsg)
{
    if (!db || !sql) {
        if (errmsg) *errmsg = strdup("Invalid database handle or SQL string.");
        return FLEXQL_ERROR;
    }

    // Collapse newlines in SQL so the single-line protocol stays intact
    string tosend = sql;
    for (char& c : tosend) if (c == '\n' || c == '\r') c = ' ';
    tosend += '\n';

    if (!sendAll(db->sockfd, tosend)) {
        if (errmsg) *errmsg = strdup("Connection broken while sending query.");
        return FLEXQL_ERROR;
    }

    string line;
    if (!recvLine(db, line)) {
        if (errmsg) *errmsg = strdup("Connection broken while reading response.");
        return FLEXQL_ERROR;
    }

    auto parts = splitTab(line);

    // ERR response
    if (parts[0] == Protocol::ERR) {
        if (errmsg) *errmsg = strdup((parts.size() > 1 ? parts[1] : "Unknown error").c_str());
        return FLEXQL_ERROR;
    }

    // Non-SELECT OK
    if (parts[0] == Protocol::OK) {
        if (parts.size() > 1) printf("%s\n", parts[1].c_str());
        return FLEXQL_OK;
    }

    // SELECT result: COLS header
    if (parts[0] == Protocol::COLS) {
        if (parts.size() < 2) {
            if (errmsg) *errmsg = strdup("Malformed COLS line.");
            return FLEXQL_ERROR;
        }
        int colCount = stoi(parts[1]);
        vector<string> colNames;
        for (int i = 0; i < colCount && (2+i) < (int)parts.size(); ++i)
            colNames.push_back(parts[2+i]);
        while ((int)colNames.size() < colCount) colNames.push_back("?");

        int rowCount = 0;
        bool aborted = false;

        while (recvLine(db, line)) {
            if (line == Protocol::END) break;
            parts = splitTab(line);
            if (parts.empty() || parts[0] != Protocol::ROW) continue;

            vector<string> values;
            for (int i = 0; i < colCount; ++i)
                values.push_back((i+1 < (int)parts.size()) ? parts[i+1] : "");

            if (!aborted && callback) {
                vector<char*> cVals(colCount), cNames(colCount);
                for (int i = 0; i < colCount; ++i) {
                    cVals[i]  = const_cast<char*>(values[i].c_str());
                    cNames[i] = const_cast<char*>(colNames[i].c_str());
                }
                if (callback(arg, colCount, cVals.data(), cNames.data()) != 0)
                    aborted = true;
            }
            ++rowCount;
        }
        if (rowCount == 0) printf("0 rows returned.\n");
        return FLEXQL_OK;
    }

    string msg = "Unexpected server response: " + line;
    if (errmsg) *errmsg = strdup(msg.c_str());
    return FLEXQL_ERROR;
}

// ── flexql_free ───────────────────────────────────────────────────────────────
void flexql_free(void* ptr) { free(ptr); }
