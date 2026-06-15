#pragma once
#include <string>
#include <fstream>
#include <iostream>
#include "json.hpp"

struct DBConfig {
    std::string host;
    std::string user;
    std::string password;
    std::string name;
    int port;
};

struct ServerConfig {
    int port;
};

struct HeartbeatConfig {
    int ping_interval_sec;
    int timeout_sec;
    int check_interval_sec;
};

struct RAGConfig {
    bool enabled;
    std::string mode; // "fts5" or "chroma"
    std::string embedding_url;
    std::string chroma_url;
    std::string collection_name;
    int top_k;
    int sync_interval_sec;
    int max_tokens_for_rag;
};

struct LLMConfig {
    std::string api_key;
    std::string api_url;
    std::string model;
    std::string embedding_model;
    std::string vector_db_url;
    int context_length;
    int rag_top_k;
    int vectorize_interval_min;
};

struct AppConfig {
    DBConfig db;
    ServerConfig server;
    HeartbeatConfig heartbeat;
    LLMConfig llm;
    RAGConfig rag;
};

inline AppConfig loadConfig(const std::string& path = "config.json") {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[ERROR] Cannot open config file: " << path << std::endl;
        exit(1);
    }
    nlohmann::json j;
    f >> j;
    AppConfig cfg;
    cfg.db.host = j["database"]["host"];
    cfg.db.user = j["database"]["user"];
    cfg.db.password = j["database"]["password"];
    cfg.db.name = j["database"]["name"];
    cfg.db.port = j["database"]["port"];
    cfg.server.port = j["server"]["port"];
    cfg.heartbeat.ping_interval_sec = j["heartbeat"]["ping_interval_sec"];
    cfg.heartbeat.timeout_sec = j["heartbeat"]["timeout_sec"];
    cfg.heartbeat.check_interval_sec = j["heartbeat"]["check_interval_sec"];
    
    if (j.contains("llm")) {
        cfg.llm.api_key = j["llm"]["api_key"];
        cfg.llm.api_url = j["llm"]["api_url"];
        cfg.llm.model = j["llm"]["model"];
        cfg.llm.embedding_model = j["llm"]["embedding_model"];
        cfg.llm.vector_db_url = j["llm"]["vector_db_url"];
        cfg.llm.context_length = j["llm"]["context_length"];
        cfg.llm.rag_top_k = j["llm"]["rag_top_k"];
        cfg.llm.vectorize_interval_min = j["llm"]["vectorize_interval_min"];
    }
    
    if (j.contains("rag")) {
        cfg.rag.enabled = j["rag"]["enabled"];
        cfg.rag.mode = j["rag"]["mode"];
        cfg.rag.embedding_url = j["rag"]["embedding_url"];
        cfg.rag.chroma_url = j["rag"]["chroma_url"];
        cfg.rag.collection_name = j["rag"]["collection_name"];
        cfg.rag.top_k = j["rag"]["top_k"];
        cfg.rag.sync_interval_sec = j["rag"]["sync_interval_sec"];
        cfg.rag.max_tokens_for_rag = j["rag"]["max_tokens_for_rag"];
    } else {
        cfg.rag.enabled = false;
        cfg.rag.mode = "fts5";
        cfg.rag.top_k = 5;
        cfg.rag.sync_interval_sec = 300;
        cfg.rag.max_tokens_for_rag = 1000;
    }
    
    return cfg;
}