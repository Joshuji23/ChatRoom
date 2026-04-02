#pragma once
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <string>
#include <iostream>

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }
    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!file.is_open()) return;
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        localtime_s(&tm_buf, &now_c); 
        char buf[64];
        std::strftime(buf, sizeof(buf), "[%Y-%m-%d %H:%M:%S] ", &tm_buf);
        file << buf << msg << std::endl;
        std::cout << "[LOG] " << msg << std::endl;
    }
private:
    Logger() {
        file.open("chatroom.log", std::ios::out | std::ios::app);
        if (!file.is_open()) {
            std::cerr << "无法打开日志文件" << std::endl;
        }
    }
    ~Logger() {
        if (file.is_open()) file.close();
    }
    std::ofstream file;
    std::mutex mtx;
};