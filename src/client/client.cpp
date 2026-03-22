/*
 * src/client/client.cpp
 * Interactive REPL main — uses flexql C API.
 */
#include "../../include/flexql.h"
#include "../../include/network/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <iostream>

using namespace std;

// ── REPL main ─────────────────────────────────────────────────────────────────

static int printCallback(void*, int colCount, char** values, char** colNames) {
    for (int i = 0; i < colCount; ++i)
        printf("%s = %s\n", colNames[i], values[i]);
    printf("\n");
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }
    FlexQL* db = nullptr;
    if (flexql_open(argv[1], atoi(argv[2]), &db) != FLEXQL_OK) {
        fprintf(stderr, "Cannot connect to FlexQL server at %s:%s\n", argv[1], argv[2]);
        return 1;
    }
    printf("Connected to FlexQL server\n");

    string line, buffer;
    while (true) {
        printf("%s", buffer.empty() ? "flexql> " : "     -> ");
        fflush(stdout);
        if (!getline(cin, line)) break;
        buffer += line;

        string trimmed = buffer;
        while (!trimmed.empty() && isspace((unsigned char)trimmed.back())) trimmed.pop_back();
        string lo = trimmed;
        transform(lo.begin(), lo.end(), lo.begin(), ::tolower);

        bool isExit = (lo==".exit"||lo==".quit"||lo=="exit"||lo=="quit");
        bool isDone = isExit || (!trimmed.empty() && trimmed.back() == ';');

        if (!isDone) { buffer += ' '; continue; }

        if (isExit) {
            flexql_close(db);
            printf("Connection closed\n");
            return 0;
        }

        char* errmsg = nullptr;
        if (flexql_exec(db, buffer.c_str(), printCallback, nullptr, &errmsg) != FLEXQL_OK) {
            if (errmsg) { fprintf(stderr, "Error: %s\n", errmsg); flexql_free(errmsg); }
        }
        buffer.clear();
    }
    flexql_close(db);
    return 0;
}
