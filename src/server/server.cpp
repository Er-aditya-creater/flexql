/*
 * src/server/server.cpp
 * =====================
 * FlexQL TCP server — accepts connections, dispatches to Engine.
 * Each client gets its own thread; g_engineMutex prevents data races.
 *
 * Compile via Makefile.
 * Run: ./bin/flexql-server 9000
 */
#include "../../include/query/engine.h"
#include "../../include/network/protocol.h"
#include "../../include/concurrency/mutex_guard.h"

#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

using namespace std;

// ── Socket helpers ────────────────────────────────────────────────────────────

// Buffered recvLine: reads up to 65536 bytes at once from the socket
// instead of one character at a time. Critical for large SQL statements
// (e.g. 5000-row batch INSERTs ~350KB). Speed improvement: ~100x.
static bool recvLine(int fd, string& out) {
    static thread_local string buf;
    out.clear();
    while (true) {
        size_t pos = buf.find('\n');
        if (pos != string::npos) {
            out = buf.substr(0, pos);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            buf.erase(0, pos + 1);
            return true;
        }
        char tmp[65536];
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            // Connection closed: flush remaining buffer as last line
            if (!buf.empty()) { out = buf; buf.clear(); return !out.empty(); }
            return false;
        }
        buf.append(tmp, n);
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

// ── Thread-safe engine call ───────────────────────────────────────────────────

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static QueryResult safeExecute(Engine& engine, const string& sql) {
    MutexGuard guard(g_mutex);
    return engine.execute(sql);
}

// ── Client handler ────────────────────────────────────────────────────────────

static void handleClient(int fd, Engine& engine) {
    string sql;
    while (recvLine(fd, sql)) {
        if (sql.empty()) continue;
        if (sql == Protocol::QUIT) break;

        QueryResult qr = safeExecute(engine, sql);

        string resp;
        if (qr.message == "__CLOSE__") {
            sendAll(fd, string(Protocol::OK) + Protocol::SEP + "Connection closed\n");
            break;
        } else if (!qr.ok) {
            resp = string(Protocol::ERR) + Protocol::SEP + qr.message + "\n";
        } else if (qr.isSelect) {
            resp = string(Protocol::COLS) + Protocol::SEP + to_string(qr.colNames.size());
            for (const auto& c : qr.colNames) resp += Protocol::SEP + c;
            resp += "\n";
            for (const auto& row : qr.rows) {
                resp += Protocol::ROW;
                for (const auto& v : row) resp += Protocol::SEP + v;
                resp += "\n";
            }
            resp += string(Protocol::END) + "\n";
        } else {
            resp = string(Protocol::OK) + Protocol::SEP + qr.message + "\n";
        }
        if (!sendAll(fd, resp)) break;
    }
    close(fd);
}

// ── Thread function ───────────────────────────────────────────────────────────

struct ClientCtx { int fd; Engine* engine; };

void* threadFunc(void* arg) {
    ClientCtx* ctx = static_cast<ClientCtx*>(arg);
    handleClient(ctx->fd, *ctx->engine);
    delete ctx;
    return nullptr;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int port = 9000;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(serverFd, 16) < 0) { perror("listen"); return 1; }

    cout << "FlexQL server started on port " << port << ".\n"
         << "Waiting for connections...\n";

    Engine engine;

    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &len);
        if (clientFd < 0) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, INET_ADDRSTRLEN);
        cout << "Client connected: " << ip << "\n";

        ClientCtx* ctx = new ClientCtx{clientFd, &engine};
        pthread_t tid;
        pthread_create(&tid, nullptr, threadFunc, ctx);
        pthread_detach(tid);
    }

    close(serverFd);
    return 0;
}
