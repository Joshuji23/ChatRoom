#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>

class RAGFTS {
public:
    RAGFTS(const std::string& dbPath = "rag_index.db");
    ~RAGFTS();
    
    bool init();
    bool syncMessagesFromMySQL(int lastSyncedId, int& newLastSyncedId);
    std::vector<std::string> searchSimilar(const std::string& query, int roomId, int topK = 5);
    
    int getLastSyncedId() const { return m_lastSyncedId; }
    void setLastSyncedId(int id) { m_lastSyncedId = id; }
    
private:
    std::string m_dbPath;
    sqlite3* m_db;
    int m_lastSyncedId;
    
    bool createFTSTable();
    bool insertMessage(int msgId, int roomId, const std::string& content);
    std::string escapeFTSQuery(const std::string& query);
};