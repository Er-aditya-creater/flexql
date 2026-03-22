#pragma once
/*
 * include/network/protocol.h
 * --------------------------
 * FlexQL Wire Protocol  (newline-terminated, tab-delimited)
 *
 * Client -> Server:
 *   <sql>\n        Execute SQL
 *   QUIT\n         Close connection
 *
 * Server -> Client:
 *   OK\t<msg>\n              Non-SELECT success
 *   ERR\t<msg>\n             Any error
 *   COLS\t<n>\t<c1>\t...\n  SELECT column header
 *   ROW\t<v1>\t<v2>...\n    One result row
 *   END\n                    End of SELECT results
 */
namespace Protocol {
    constexpr const char* OK   = "OK";
    constexpr const char* ERR  = "ERR";
    constexpr const char* COLS = "COLS";
    constexpr const char* ROW  = "ROW";
    constexpr const char* END  = "END";
    constexpr const char* QUIT = "QUIT";
    constexpr char        SEP  = '\t';
}
