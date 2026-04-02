#ifndef UTILS_H
#define UTILS_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <iostream>
#include <chrono>

// 房间结构体定义
struct Room {
    std::string name;
    bool isPublic;
    std::string password;
    std::vector<SOCKET> members;
    std::vector<std::string> nicknames;
    int ownerId;
    std::map<SOCKET, time_t> mutedUntil;   // 禁言结束时间戳
};

// 全局变量声明
extern std::vector<SOCKET> clients;
extern std::map<SOCKET, std::string> clientNicknames;
extern std::map<SOCKET, int> clientUserIds;
extern std::map<SOCKET, std::string> clientRooms;
extern std::map<std::string, Room> rooms;
extern std::mutex clients_mutex;
extern std::mutex rooms_mutex;

// 心跳相关
extern std::map<SOCKET, std::chrono::steady_clock::time_point> clientLastHeartbeat;

// 函数声明
void broadcast(const std::string& message, SOCKET sender);
void broadcastToRoom(const std::string& roomName, const std::string& message, SOCKET sender);
void handle_client(SOCKET clientSocket);
void startServer();
void broadcastRoomList();
void broadcastOnlineCount();

#endif