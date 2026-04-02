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

struct AppConfig {
    DBConfig db;
    ServerConfig server;
    HeartbeatConfig heartbeat;
};

inline AppConfig loadConfig(const std::string& path = "config.json") {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[ERROR] 无法打开配置文件: " << path << std::endl;
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
    return cfg;
}