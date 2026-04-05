#ifndef UTILS_H
#define UTILS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <chrono>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "win_mutex.h"
#include "win_thread.h"

typedef WinMutex Mutex;
typedef WinLockGuard<WinMutex> LockGuard;
typedef WinThread Thread;

struct Room {
    std::string name;
    bool isPublic;
    std::string password;
    std::vector<SOCKET> members;
    std::vector<std::string> nicknames;
    int ownerId;
    std::map<SOCKET, time_t> mutedUntil;
};

extern std::vector<SOCKET> clients;
extern std::map<SOCKET, std::string> clientNicknames;
extern std::map<SOCKET, int> clientUserIds;
extern std::map<SOCKET, std::string> clientRooms;
extern std::map<std::string, Room> rooms;
extern Mutex clients_mutex;
extern Mutex rooms_mutex;

extern std::map<SOCKET, std::chrono::steady_clock::time_point> clientLastHeartbeat;

void broadcast(const std::string& message, SOCKET sender);
void broadcastToRoom(const std::string& roomName, const std::string& message, SOCKET sender);
void handle_client(SOCKET clientSocket);
void startServer();
void broadcastRoomList();
void broadcastOnlineCount();

#endif
