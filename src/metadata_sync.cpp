#include "xTdb/metadata_sync.h"
#include <cstring>
#include <chrono>

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
        return SyncResult::ERR_DB_OPEN_FAILED;
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
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::initSchema() {
    // Create chunks table
    std::string create_chunks = R"(
        CREATE TABLE IF NOT EXISTS chunks (
            chunk_id INTEGER PRIMARY KEY,
            container_id INTEGER NOT NULL DEFAULT 0,
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

    // Create blocks table (extended for COMPACT archive support)
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
            container_id INTEGER NOT NULL DEFAULT 0,
            is_archived INTEGER NOT NULL DEFAULT 0,
            archived_to_container_id INTEGER,
            archived_to_block_index INTEGER,
            original_chunk_id INTEGER,
            original_block_index INTEGER,
            encoding_type INTEGER DEFAULT 0,
            original_size INTEGER DEFAULT 0,
            compressed_size INTEGER DEFAULT 0,
            FOREIGN KEY (chunk_id) REFERENCES chunks(chunk_id)
        );
    )";

    result = executeSql(create_blocks);
    if (result != SyncResult::SUCCESS) {
        return result;
    }

    // Migrate existing tables if necessary (add new columns)
    // SQLite will ignore ALTER TABLE if column already exists
    executeSql("ALTER TABLE blocks ADD COLUMN container_id INTEGER NOT NULL DEFAULT 0;");
    executeSql("ALTER TABLE blocks ADD COLUMN is_archived INTEGER NOT NULL DEFAULT 0;");
    executeSql("ALTER TABLE blocks ADD COLUMN archived_to_container_id INTEGER;");
    executeSql("ALTER TABLE blocks ADD COLUMN archived_to_block_index INTEGER;");
    executeSql("ALTER TABLE blocks ADD COLUMN original_chunk_id INTEGER;");
    executeSql("ALTER TABLE blocks ADD COLUMN original_block_index INTEGER;");
    executeSql("ALTER TABLE blocks ADD COLUMN encoding_type INTEGER DEFAULT 0;");
    executeSql("ALTER TABLE blocks ADD COLUMN original_size INTEGER DEFAULT 0;");
    executeSql("ALTER TABLE blocks ADD COLUMN compressed_size INTEGER DEFAULT 0;");

    // Create indexes for efficient queries
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_tag ON blocks(tag_id);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_time ON blocks(start_ts_us, end_ts_us);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_tag_time ON blocks(tag_id, start_ts_us, end_ts_us);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_container ON blocks(container_id);");
    executeSql("CREATE INDEX IF NOT EXISTS idx_blocks_archived ON blocks(is_archived, container_id);");

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
        (chunk_id, container_id, chunk_offset, start_ts_us, end_ts_us, super_crc32, is_sealed, block_count)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )";

    int rc = sqlite3_prepare_v2(db_, insert_chunk.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        executeSql("ROLLBACK;");
        setError("Failed to prepare chunk insert: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, scanned_chunk.chunk_id);
    sqlite3_bind_int(stmt, 2, 0);  // container_id = 0
    sqlite3_bind_int64(stmt, 3, chunk_offset);
    sqlite3_bind_int64(stmt, 4, scanned_chunk.start_ts_us);
    sqlite3_bind_int64(stmt, 5, scanned_chunk.end_ts_us);
    sqlite3_bind_int(stmt, 6, scanned_chunk.super_crc32);
    sqlite3_bind_int(stmt, 7, scanned_chunk.is_sealed ? 1 : 0);
    sqlite3_bind_int(stmt, 8, (int)scanned_chunk.blocks.size());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        executeSql("ROLLBACK;");
        setError("Failed to insert chunk: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
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
            return SyncResult::ERR_DB_PREPARE_FAILED;
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
            return SyncResult::ERR_DB_EXEC_FAILED;
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
        return SyncResult::ERR_DB_PREPARE_FAILED;
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
        return SyncResult::ERR_DB_EXEC_FAILED;
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
        return SyncResult::ERR_DB_PREPARE_FAILED;
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
        return SyncResult::ERR_DB_EXEC_FAILED;
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
        return SyncResult::ERR_DB_PREPARE_FAILED;
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
        return SyncResult::ERR_DB_EXEC_FAILED;
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
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        tag_ids.push_back(sqlite3_column_int(stmt, 0));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

void MetadataSync::setError(const std::string& message) {
    last_error_ = message;
}

// ============================================================================
// Phase 10: Retention Service Support
// ============================================================================

SyncResult MetadataSync::querySealedChunks(uint32_t container_id,
                                          int64_t min_end_ts,
                                          int64_t max_end_ts,
                                          std::function<void(uint32_t, uint64_t, int64_t, int64_t)> callback) {
    if (!db_) {
        setError("Database not open");
        return SyncResult::ERR_DB_OPEN_FAILED;
    }

    // Query chunks table for sealed chunks in time range
    const char* sql = "SELECT chunk_id, chunk_offset, start_ts_us, end_ts_us "
                     "FROM chunks "
                     "WHERE container_id = ? AND is_sealed = 1 ";

    std::string query = sql;
    if (min_end_ts > 0) {
        query += "AND end_ts_us >= ? ";
    }
    if (max_end_ts > 0) {
        query += "AND end_ts_us <= ? ";
    }
    query += "ORDER BY end_ts_us ASC";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    // Bind parameters
    int bind_index = 1;
    sqlite3_bind_int(stmt, bind_index++, container_id);
    if (min_end_ts > 0) {
        sqlite3_bind_int64(stmt, bind_index++, min_end_ts);
    }
    if (max_end_ts > 0) {
        sqlite3_bind_int64(stmt, bind_index++, max_end_ts);
    }

    // Execute and call callback for each row
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint32_t chunk_id = sqlite3_column_int(stmt, 0);
        uint64_t chunk_offset = sqlite3_column_int64(stmt, 1);
        int64_t start_ts = sqlite3_column_int64(stmt, 2);
        int64_t end_ts = sqlite3_column_int64(stmt, 3);

        callback(chunk_id, chunk_offset, start_ts, end_ts);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Query execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::deleteChunk(uint32_t container_id, uint32_t chunk_id) {
    if (!db_) {
        setError("Database not open");
        return SyncResult::ERR_DB_OPEN_FAILED;
    }

    // Delete from chunks table
    const char* sql = "DELETE FROM chunks WHERE container_id = ? AND chunk_id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare delete: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, container_id);
    sqlite3_bind_int(stmt, 2, chunk_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Delete execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    // Also delete related blocks
    // Note: In production, this should use foreign key cascades
    const char* block_sql = "DELETE FROM blocks WHERE chunk_id = ?";

    rc = sqlite3_prepare_v2(db_, block_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare block delete: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, chunk_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Block delete execution failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

// ============================================================================
// Phase 18/19: COMPACT Archive Support
// ============================================================================

SyncResult MetadataSync::syncCompactBlock(uint32_t container_id,
                                         uint32_t block_index,
                                         uint32_t tag_id,
                                         uint32_t original_chunk_id,
                                         uint32_t original_block_index,
                                         int64_t start_ts_us,
                                         int64_t end_ts_us,
                                         uint32_t record_count,
                                         EncodingType original_encoding,
                                         ValueType value_type,
                                         TimeUnit time_unit,
                                         uint32_t original_size,
                                         uint32_t compressed_size) {
    const char* sql = R"(
        INSERT INTO blocks
        (chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
         time_unit, value_type, record_count, chunk_offset,
         container_id, is_archived, original_chunk_id, original_block_index,
         encoding_type, original_size, compressed_size)
        VALUES (0, ?, ?, ?, ?, ?, ?, ?, 0, ?, 0, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare COMPACT block insert: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    // Bind parameters
    sqlite3_bind_int(stmt, 1, block_index);
    sqlite3_bind_int(stmt, 2, tag_id);
    sqlite3_bind_int64(stmt, 3, start_ts_us);
    sqlite3_bind_int64(stmt, 4, end_ts_us);
    sqlite3_bind_int(stmt, 5, static_cast<int>(time_unit));
    sqlite3_bind_int(stmt, 6, static_cast<int>(value_type));
    sqlite3_bind_int(stmt, 7, record_count);
    sqlite3_bind_int(stmt, 8, container_id);  // COMPACT container ID
    sqlite3_bind_int(stmt, 9, original_chunk_id);
    sqlite3_bind_int(stmt, 10, original_block_index);
    sqlite3_bind_int(stmt, 11, static_cast<int>(original_encoding));
    sqlite3_bind_int(stmt, 12, original_size);
    sqlite3_bind_int(stmt, 13, compressed_size);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("COMPACT block insert failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::markBlockAsArchived(uint32_t raw_container_id,
                                            uint32_t chunk_id,
                                            uint32_t block_index,
                                            uint32_t archived_to_container_id,
                                            uint32_t archived_to_block_index) {
    const char* sql = R"(
        UPDATE blocks
        SET is_archived = 1,
            archived_to_container_id = ?,
            archived_to_block_index = ?
        WHERE container_id = ? AND chunk_id = ? AND block_index = ?;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare archive mark: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, archived_to_container_id);
    sqlite3_bind_int(stmt, 2, archived_to_block_index);
    sqlite3_bind_int(stmt, 3, raw_container_id);
    sqlite3_bind_int(stmt, 4, chunk_id);
    sqlite3_bind_int(stmt, 5, block_index);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Archive mark failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

SyncResult MetadataSync::queryBlocksForArchive(uint32_t raw_container_id,
                                              int64_t min_age_seconds,
                                              std::vector<BlockQueryResult>& results) {
    results.clear();

    // Query blocks that are:
    // 1. In RAW container
    // 2. Not yet archived
    // 3. Older than min_age_seconds
    int64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t cutoff_time_us = current_time_us - (min_age_seconds * 1000000LL);

    const char* sql = R"(
        SELECT chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
               time_unit, value_type, record_count, chunk_offset
        FROM blocks
        WHERE container_id = ?
          AND is_archived = 0
          AND end_ts_us < ?
        ORDER BY end_ts_us;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        setError("Failed to prepare archive query: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_PREPARE_FAILED;
    }

    sqlite3_bind_int(stmt, 1, raw_container_id);
    sqlite3_bind_int64(stmt, 2, cutoff_time_us);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        BlockQueryResult block;
        block.chunk_id = sqlite3_column_int(stmt, 0);
        block.block_index = sqlite3_column_int(stmt, 1);
        block.tag_id = sqlite3_column_int(stmt, 2);
        block.start_ts_us = sqlite3_column_int64(stmt, 3);
        block.end_ts_us = sqlite3_column_int64(stmt, 4);
        block.time_unit = static_cast<TimeUnit>(sqlite3_column_int(stmt, 5));
        block.value_type = static_cast<ValueType>(sqlite3_column_int(stmt, 6));
        block.record_count = sqlite3_column_int(stmt, 7);
        block.chunk_offset = sqlite3_column_int64(stmt, 8);
        results.push_back(block);
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        setError("Archive query failed: " + std::string(sqlite3_errmsg(db_)));
        return SyncResult::ERR_DB_EXEC_FAILED;
    }

    return SyncResult::SUCCESS;
}

}  // namespace xtdb
