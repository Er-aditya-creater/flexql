#pragma once
/*
 * include/storage/storage.h
 * =========================
 * Persistent storage for FlexQL: WAL + Snapshots.
 *
 * Architecture (inspired by PostgreSQL/SQLite WAL mode):
 * ────────────────────────────────────────────────────────
 *
 *  WRITE PATH:
 *    1. Append operation to WAL file (fsync for durability)
 *    2. Apply to in-memory Database
 *    → Even if the process crashes after step 1, the WAL can replay the op.
 *
 *  READ PATH:
 *    Served entirely from in-memory Database (fast).
 *
 *  RECOVERY (on startup):
 *    1. Load the latest snapshot from data/tables/<name>.snap
 *    2. Replay any WAL records written after that snapshot's LSN
 *    → Database is fully reconstructed without data loss.
 *
 *  CHECKPOINT (periodic):
 *    1. Write a binary snapshot of every table to data/tables/
 *    2. Record the current LSN in the snapshot header
 *    3. Truncate the WAL up to that LSN
 *    → Keeps WAL small; speeds up future recovery.
 *
 * WAL Record Binary Format:
 *   [type:1B][payload_len:4B LE][payload:N B][crc32:4B LE]
 *
 * Record types:
 *   0x01  CREATE_TABLE  table_name + schema
 *   0x02  INSERT        table_name + row (cells + expires)
 *   0x03  DELETE_ALL    table_name
 *   0x04  DELETE_WHERE  table_name + condition (colRef, op, value)
 *   0xFF  CHECKPOINT    LSN of corresponding snapshot
 *
 * Snapshot Binary Format (data/tables/<name>.snap):
 *   Magic "FLEXSNAP" | version(2B) | lsn(8B) | col_count(2B) |
 *   schema: [name_len(2B) | name | col_type(1B)] * col_count |
 *   row_count(8B) |
 *   rows: [expires(8B) | [cell_type(1B) | cell_data] * col_count] * row_count
 *
 * Cell encoding:
 *   0x01  INT/DATETIME  value(8B LE int64)
 *   0x02  DECIMAL       value(8B IEEE 754 double)
 *   0x03  VARCHAR       len(4B LE) + utf8 bytes
 */
#include "../common/types.h"
#include <string>
#include <cstdint>

// ── LSN (Log Sequence Number) ─────────────────────────────────────────────────
// Monotonically increasing counter; every WAL record gets a unique LSN.
using LSN = uint64_t;

// ── WAL record types ──────────────────────────────────────────────────────────
enum class WalType : uint8_t {
    CREATE_TABLE  = 0x01,
    INSERT        = 0x02,
    DELETE_ALL    = 0x03,
    DELETE_WHERE  = 0x04,
    INSERT_BATCH  = 0x05,   // multiple rows in one fsync
    CHECKPOINT    = 0xFF
};

// ── StorageManager ────────────────────────────────────────────────────────────
/*
 * Single class responsible for:
 *   - WAL append and fsync
 *   - Snapshot save and load
 *   - Recovery (load snapshot + replay WAL)
 *   - Checkpoint (snapshot + WAL truncation)
 */
class StorageManager {
    std::string dataDir_;    // path to data/tables/
    std::string walPath_;    // path to data/wal/flexql.wal
    int         walFd_ = -1; // open WAL file descriptor (append mode)
    LSN         currentLSN_ = 0;

    // ── Binary serialization helpers ──────────────────────────────────────────
    static void writeU8 (std::string& buf, uint8_t  v);
    static void writeU16(std::string& buf, uint16_t v);
    static void writeU32(std::string& buf, uint32_t v);
    static void writeU64(std::string& buf, uint64_t v);
    static void writeStr(std::string& buf, const std::string& s);
    static void writeCell(std::string& buf, const CellValue& cv, ColType ct);

    static uint8_t  readU8 (const char*& p);
    static uint16_t readU16(const char*& p);
    static uint32_t readU32(const char*& p);
    static uint64_t readU64(const char*& p);
    static std::string readStr(const char*& p);
    static CellValue   readCell(const char*& p, ColType ct);

    static uint32_t crc32(const void* data, size_t len);

    // ── WAL internals ─────────────────────────────────────────────────────────
    bool appendWAL(WalType type, const std::string& payload);

    // ── Snapshot internals ────────────────────────────────────────────────────
    bool saveSnapshot(const Table& tbl, LSN lsn);
    bool loadSnapshot(const std::string& tableName, Table& tbl, LSN& snapshotLSN);

public:
    StorageManager() = default;
    ~StorageManager();

    // Call once at startup; opens/creates WAL file.
    bool open(const std::string& dataDir, const std::string& walPath);

    // ── WAL write operations (called by Engine before modifying memory) ───────
    bool walCreateTable(const Table& tbl);
    bool walInsert     (const std::string& tableName, const Row& row,
                        const std::vector<ColType>& colTypes);
    bool walDeleteAll  (const std::string& tableName);
    // Write entire batch of rows in a single WAL record (one fsync per batch)
    bool walInsertBatch(const std::string& tableName,
                        const std::vector<Row>& rows,
                        const std::vector<ColType>& colTypes);

    // ── Recovery ──────────────────────────────────────────────────────────────
    // Loads snapshots + replays WAL into `db`. Called once on startup.
    bool recover(Database& db);

    // ── Checkpoint ────────────────────────────────────────────────────────────
    // Saves snapshots for all tables, then truncates the WAL.
    bool checkpoint(const Database& db);

    LSN currentLSN() const { return currentLSN_; }
};
