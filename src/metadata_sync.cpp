#include "xTdb/metadata_sync.h"
#include <cstring>

namespace xtdb {

MetadataSync::MetadataSync(const std::string& db_path)
    : db_path_(db_path), db_(nullptr) {
}

MetadataSync::~MetadataSync() {
    close();
}

SyncResult MetadataSync::open() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        setError("Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        sqlite3_close(db_);
        db_ = nullptr;
        return SyncResult::ERROR_DB_OPEN_FAILED;
    }

    // Enable WAL mode for better concurrency
    executeSql("PRAGMA journal_mode=WAL");

    return SyncResult::SUCCESS;
}

void MetadataSync::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

SyncResult MetadataSync::executeSql(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        setError("SQL execution failed: " + error);
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::initSchema() {
    // Create chunks table
    std::string create_chunks = R"(
        CREATE TABLE IF NOT EXISTS chunks (
            chunk_id INTEGER PRIMARY KEY,
            chunk_offset INTEGER NOT NULL,
            start_ts_us INTEGER NOT NULL,
            end_ts_us INTEGER NOT NULL,
            super_crc32 INTEGER NOT NULL,
            is_sealed INTEGER NOT NULL,
            block_count INTEGER NOT NULL
        );
    )";

    SyncResult result = executeSql(create_chunks);
    if (result != SyncResult::SUCCESS) {
        return result;
    }

    // Create blocks table
    std::string create_blocks = R"(
        CREATE TABLE IF NOT EXISTS blocks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            chunk_id INTEGER NOT NULL,
            block_index INTEGER NOT NULL,
            tag_id INTEGER NOT NULL,
            start_ts_us INTEGER NOT NULL,
            end_ts_us INTEGER NOT NULL,
            time_unit INTEGER NOT NULL,
            value_type INTEGER NOT NULL,
            record_count INTEGER NOT NULL,
            chunk_offset INTEGER NOT NULL,
            FOREIGN KEY (chunk_id) REFERENCES chunks(chunk_id)
        );
    )";

    result = executeSql(create_blocks);
    if (result != SyncResult::SUCCESS) {
        return result;
    }

    // Create indexes for efficient queries
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_tag ON blocks(tag_id);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_time ON blocks(start_ts_us, end_ts_us);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_tag_time ON blocks(tag_id, start_ts_us, end_ts_us);");

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::syncChunk(uint64_t chunk_offset,
                                  const ScannedChunk& scanned_chunk) {
    // Begin transaction
    SyncResult result = executeSql("BEGIN TRANSACTION;");
    if (result != SyncResult::SUCCESS) {
        return result;
    }

    // Insert chunk
    sqlite3_stmt* stmt = nullptr;
    std::string insert_chunk = R"(
        INSERT OR REPLACE INTO chunks
        (chunk_id, chunk_offset, start_ts_us, end_ts_us, super_crc32, is_sealed, block_count)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )";

    int rc = sqlite3_prepare_v2(db_, insert_chunk.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        executeSql("ROLLBACK;");
        setError("Failed to prepare chunk insert: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, scanned_chunk.chunk_id);
    sqlite3_bind_int64(stmt, 2, chunk_offset);
    sqlite3_bind_int64(stmt, 3, scanned_chunk.start_ts_us);
    sqlite3_bind_int64(stmt, 4, scanned_chunk.end_ts_us);
    sqlite3_bind_int(stmt, 5, scanned_chunk.super_crc32);
    sqlite3_bind_int(stmt, 6, scanned_chunk.is_sealed ? 1 : 0);
    sqlite3_bind_int(stmt, 7, scanned_chunk.blocks.size());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        executeSql("ROLLBACK;");
        setError("Failed to insert chunk: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    // Insert blocks
    std::string insert_block = R"(
        INSERT INTO blocks
        (chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
         time_unit, value_type, record_count, chunk_offset)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    for (const auto& block : scanned_chunk.blocks) {
        rc = sqlite3_prepare_v2(db_, insert_block.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            executeSql("ROLLBACK;");
            setError("Failed to prepare block insert: " + std::string(sqlite3_errmsg(db_)));
            return SyncResult::ERROR_DB_PREPARE_FAILED;
        }

        sqlite3_bind_int(stmt, 1, scanned_chunk.chunk_id);
        sqlite3_bind_int(stmt, 2, block.block_index);
        sqlite3_bind_int(stmt, 3, block.tag_id);
        sqlite3_bind_int64(stmt, 4, block.start_ts_us);
        sqlite3_bind_int64(stmt, 5, block.end_ts_us);
        sqlite3_bind_int(stmt, 6, static_cast<int>(block.time_unit));
        sqlite3_bind_int(stmt, 7, static_cast<int>(block.value_type));
        sqlite3_bind_int(stmt, 8, block.record_count);
        sqlite3_bind_int64(stmt, 9, chunk_offset);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            executeSql("ROLLBACK;");
            setError("Failed to insert block: " + std::string(sqlite3_errmsg(db_)));
            return SyncResult::ERROR_DB_EXEC_FAILED;
        }
    }

    // Commit transaction
    result = executeSql("COMMIT;");
    return result;
}

SyncResult MetadataSync::queryBlocksByTag(uint32_t tag_id,
                                         std::vector<BlockQueryResult>& results) {
    results.clear();

    std::string query = R"(
        SELECT chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
               time_unit, value_type, record_count, chunk_offset
        FROM blocks
        WHERE tag_id = ?
        ORDER BY start_ts_us;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, tag_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        BlockQueryResult result;
        result.chunk_id = sqlite3_column_int(stmt, 0);
        result.block_index = sqlite3_column_int(stmt, 1);
        result.tag_id = sqlite3_column_int(stmt, 2);
        result.start_ts_us = sqlite3_column_int64(stmt, 3);
        result.end_ts_us = sqlite3_column_int64(stmt, 4);
        result.time_unit = static_cast<TimeUnit>(sqlite3_column_int(stmt, 5));
        result.value_type = static_cast<ValueType>(sqlite3_column_int(stmt, 6));
        result.record_count = sqlite3_column_int(stmt, 7);
        result.chunk_offset = sqlite3_column_int64(stmt, 8);

        results.push_back(result);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::queryBlocksByTimeRange(int64_t start_ts_us,
                                               int64_t end_ts_us,
                                               std::vector<BlockQueryResult>& results) {
    results.clear();

    std::string query = R"(
        SELECT chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
               time_unit, value_type, record_count, chunk_offset
        FROM blocks
        WHERE start_ts_us <= ? AND end_ts_us >= ?
        ORDER BY start_ts_us;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int64(stmt, 1, end_ts_us);
    sqlite3_bind_int64(stmt, 2, start_ts_us);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        BlockQueryResult result;
        result.chunk_id = sqlite3_column_int(stmt, 0);
        result.block_index = sqlite3_column_int(stmt, 1);
        result.tag_id = sqlite3_column_int(stmt, 2);
        result.start_ts_us = sqlite3_column_int64(stmt, 3);
        result.end_ts_us = sqlite3_column_int64(stmt, 4);
        result.time_unit = static_cast<TimeUnit>(sqlite3_column_int(stmt, 5));
        result.value_type = static_cast<ValueType>(sqlite3_column_int(stmt, 6));
        result.record_count = sqlite3_column_int(stmt, 7);
        result.chunk_offset = sqlite3_column_int64(stmt, 8);

        results.push_back(result);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::queryBlocksByTagAndTime(uint32_t tag_id,
                                                int64_t start_ts_us,
                                                int64_t end_ts_us,
                                                std::vector<BlockQueryResult>& results) {
    results.clear();

    std::string query = R"(
        SELECT chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
               time_unit, value_type, record_count, chunk_offset
        FROM blocks
        WHERE tag_id = ? AND start_ts_us <= ? AND end_ts_us >= ?
        ORDER BY start_ts_us;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, tag_id);
    sqlite3_bind_int64(stmt, 2, end_ts_us);
    sqlite3_bind_int64(stmt, 3, start_ts_us);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        BlockQueryResult result;
        result.chunk_id = sqlite3_column_int(stmt, 0);
        result.block_index = sqlite3_column_int(stmt, 1);
        result.tag_id = sqlite3_column_int(stmt, 2);
        result.start_ts_us = sqlite3_column_int64(stmt, 3);
        result.end_ts_us = sqlite3_column_int64(stmt, 4);
        result.time_unit = static_cast<TimeUnit>(sqlite3_column_int(stmt, 5));
        result.value_type = static_cast<ValueType>(sqlite3_column_int(stmt, 6));
        result.record_count = sqlite3_column_int(stmt, 7);
        result.chunk_offset = sqlite3_column_int64(stmt, 8);

        results.push_back(result);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::getAllTags(std::vector<uint32_t>& tag_ids) {
    tag_ids.clear();

    std::string query = "SELECT DISTINCT tag_id FROM blocks ORDER BY tag_id;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_PREPARE_FAILED;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        tag_ids.push_back(sqlite3_column_int(stmt, 0));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERROR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

void MetadataSync::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
