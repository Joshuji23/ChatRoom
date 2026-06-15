#define SQLITE_ENABLE_FTS5 1
#include "sqlite3.h"
#include "rag_fts.h"
#include "database.h"
#include "logger.h"
#include <sstream>
#include <algorithm>

RAGFTS::RAGFTS(const std::string& dbPath)
    : m_dbPath(dbPath), m_db(nullptr), m_lastSyncedId(0) {
}

RAGFTS::~RAGFTS() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

bool RAGFTS::init() {
    int rc = sqlite3_open(m_dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        Logger::instance().log("[RAG ERROR] Cannot open RAG database: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    if (!createFTSTable()) {
        return false;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT MAX(msg_id) FROM messages_fts";
    rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            m_lastSyncedId = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    Logger::instance().log("[RAG] FTS5 initialized, last synced ID: " + std::to_string(m_lastSyncedId));
    return true;
}

bool RAGFTS::createFTSTable() {
    const char* sql = "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
                      "content, "
                      "room_id UNINDEXED, "
                      "msg_id UNINDEXED, "
                      "tokenize = 'unicode61' "
                      ")";
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        Logger::instance().log("[RAG ERROR] Failed to create FTS5 table: " + std::string(errMsg));
        sqlite3_free(errMsg);
        return false;
    }
    
    return true;
}

bool RAGFTS::syncMessagesFromMySQL(int lastSyncedId, int& newLastSyncedId) {
    MYSQL* conn = connectDB();
    if (!conn) {
        Logger::instance().log("[RAG ERROR] Failed to connect to MySQL for RAG sync");
        return false;
    }
    
    std::string query = "SELECT id, room_id, content FROM messages WHERE id > " + 
                        std::to_string(lastSyncedId) + " ORDER BY id LIMIT 100";
    
    if (mysql_query(conn, query.c_str()) != 0) {
        Logger::instance().log("[RAG ERROR] MySQL query failed: " + std::string(mysql_error(conn)));
        mysql_close(conn);
        return false;
    }
    
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        mysql_close(conn);
        return false;
    }
    
    int syncedCount = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        int msgId = atoi(row[0]);
        int roomId = atoi(row[1]);
        std::string content = row[2] ? row[2] : "";
        
        if (insertMessage(msgId, roomId, content)) {
            m_lastSyncedId = msgId;
            syncedCount++;
        }
    }
    
    mysql_free_result(res);
    mysql_close(conn);
    
    newLastSyncedId = m_lastSyncedId;
    
    if (syncedCount > 0) {
        Logger::instance().log("[RAG] Synced " + std::to_string(syncedCount) + " messages");
    }
    
    return syncedCount > 0;
}

bool RAGFTS::insertMessage(int msgId, int roomId, const std::string& content) {
    const char* sql = "INSERT OR REPLACE INTO messages_fts (msg_id, room_id, content) VALUES (?, ?, ?)";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        Logger::instance().log("[RAG ERROR] Failed to prepare insert statement: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, msgId);
    sqlite3_bind_int(stmt, 2, roomId);
    sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        Logger::instance().log("[RAG ERROR] Failed to insert message: " + std::string(sqlite3_errmsg(m_db)));
        return false;
    }
    
    return true;
}

std::string RAGFTS::escapeFTSQuery(const std::string& query) {
    std::string escaped;
    for (char c : query) {
        if (c == '"' || c == '\'' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

std::vector<std::string> RAGFTS::searchSimilar(const std::string& query, int roomId, int topK) {
    std::vector<std::string> results;
    
    if (!m_db) {
        return results;
    }
    
    std::string escapedQuery = escapeFTSQuery(query);
    std::string sql = "SELECT content FROM messages_fts WHERE messages_fts MATCH ? AND room_id = ? ORDER BY rank LIMIT ?";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        Logger::instance().log("[RAG ERROR] Failed to prepare search statement: " + std::string(sqlite3_errmsg(m_db)));
        return results;
    }
    
    sqlite3_bind_text(stmt, 1, escapedQuery.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, roomId);
    sqlite3_bind_int(stmt, 3, topK);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* content = (const char*)sqlite3_column_text(stmt, 0);
        if (content) {
            results.push_back(content);
        }
    }
    
    sqlite3_finalize(stmt);
    
    Logger::instance().log("[RAG] Found " + std::string(std::to_string(results.size())) + " similar messages");
    return results;
}