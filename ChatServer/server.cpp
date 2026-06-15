#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "utils.h"
#include "database.h"
#include "config.h"
#include "logger.h"
#include "rag_fts.h"

#include <algorithm>
#include <sstream>
#include <iostream>
#include <vector>
#include <map>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <future>
#include <regex>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

const std::string LOBBY_NAME = "Lobby";

std::vector<SOCKET> clients;
std::map<SOCKET, std::string> clientNicknames;
std::map<SOCKET, int> clientUserIds;
std::map<SOCKET, std::string> clientRooms;
std::map<std::string, Room> rooms;
Mutex clients_mutex;
Mutex rooms_mutex;
std::map<SOCKET, std::chrono::steady_clock::time_point> clientLastHeartbeat;

extern AppConfig g_config;
RAGFTS* g_ragFTS = nullptr;

void handleGetHistory(SOCKET clientSocket, const std::string& roomName);

std::string getNicknameBySocket(SOCKET sock) {
    LockGuard lock(clients_mutex);
    auto it = clientNicknames.find(sock);
    if (it != clientNicknames.end()) return it->second;
    return "";
}

void handleGetMembers(SOCKET clientSocket, const std::string& roomName) {
    LockGuard lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) return;
    
    const Room& room = it->second;
    std::string memberList;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        const std::string& nick = room.nicknames[i];
        SOCKET memberSocket = room.members[i];
        
        std::string displayNick = nick;
        int memberUserId = clientUserIds[memberSocket];
        if (memberUserId == room.ownerId) {
            displayNick += "[OWNER]";
        }
        
        memberList += displayNick + ",";
    }
    if (!memberList.empty()) memberList.pop_back();
    
    std::string msg = "MEMBERS|" + roomName + "|" + memberList + "\n";
    send(clientSocket, msg.c_str(), (int)msg.size(), 0);
}

void broadcastRoomList() {
    std::string roomList;
    {
        LockGuard lock(rooms_mutex);
        for (const auto& pair : rooms) {
            const Room& room = pair.second;
            if (room.name != LOBBY_NAME) {
                roomList += room.name + "|" + (room.isPublic ? "Public" : "Private") + "|" + std::to_string(room.members.size()) + ";";
            }
        }
    }
    if (!roomList.empty()) roomList.pop_back();
    std::string response = "ROOM_LIST|" + roomList + "\n";

    LockGuard lock(clients_mutex);
    for (SOCKET client : clients) {
        send(client, response.c_str(), (int)response.size(), 0);
    }
    std::cout << "[BROADCAST] Room list: " << response << std::endl;
}

void broadcastOnlineCount() {
    int count;
    {
        LockGuard lock(clients_mutex);
        count = (int)clients.size();
    }
    std::string msg = "ONLINE_COUNT|" + std::to_string(count) + "\n";
    LockGuard lock(clients_mutex);
    for (SOCKET client : clients) {
        send(client, msg.c_str(), (int)msg.size(), 0);
    }
    std::cout << "[BROADCAST] Online count: " << count << std::endl;
}

void broadcastMemberList(const std::string& roomName) {
    LockGuard lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) return;
    
    const Room& room = it->second;
    std::string memberList;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        const std::string& nick = room.nicknames[i];
        SOCKET memberSocket = room.members[i];
        
        std::string displayNick = nick;
        int memberUserId = clientUserIds[memberSocket];
        if (memberUserId == room.ownerId) {
            displayNick += "[OWNER]";
        }
        
        memberList += displayNick + ",";
    }
    if (!memberList.empty()) memberList.pop_back();
    
    std::string msg = "MEMBERS|" + roomName + "|" + memberList + "\n";
    
    for (SOCKET member : room.members) {
        send(member, msg.c_str(), (int)msg.size(), 0);
    }
    std::cout << "[BROADCAST] Members for room " << roomName << ": " << memberList << std::endl;
}

void cleanupClient(SOCKET clientSocket) {
    std::string leaveMsg;
    std::vector<SOCKET> members;
    std::string roomName;
    std::string nickname;
    int userId;
    {
        LockGuard lock(clients_mutex);
        roomName = clientRooms[clientSocket];
        nickname = clientNicknames[clientSocket];
        userId = clientUserIds[clientSocket];
    }
    if (!roomName.empty()) {
        LockGuard lockRooms(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) {
            Room& room = it->second;
            auto pos = std::find(room.members.begin(), room.members.end(), clientSocket);
            if (pos != room.members.end()) {
                size_t idx = pos - room.members.begin();
                room.members.erase(pos);
                room.nicknames.erase(room.nicknames.begin() + idx);
                room.mutedUntil.erase(clientSocket);
                leaveMsg = "LEAVE|" + nickname + " left the room\n";
                members = room.members;
                if (room.members.empty() && room.name != LOBBY_NAME) {
                    deleteRoomFromDB(room.name);
                    rooms.erase(it);
                    Logger::instance().log("Room " + room.name + " deleted (empty)");
                }
                else {
                    removeMemberFromRoomDB(userId, roomName);
                }
            }
        }
    }
    for (SOCKET member : members) {
        if (member != clientSocket) send(member, leaveMsg.c_str(), (int)leaveMsg.size(), 0);
    }
    {
        LockGuard lock(clients_mutex);
        auto it = std::find(clients.begin(), clients.end(), clientSocket);
        if (it != clients.end()) clients.erase(it);
        clientNicknames.erase(clientSocket);
        clientUserIds.erase(clientSocket);
        clientRooms.erase(clientSocket);
        clientLastHeartbeat.erase(clientSocket);
    }
    broadcastRoomList();
    broadcastOnlineCount();
    closesocket(clientSocket);
}

std::vector<SOCKET> removeFromRoom(SOCKET clientSocket, const std::string& roomName, std::string& leaveMsg) {
    std::vector<SOCKET> members;
    try {
        LockGuard lockClients(clients_mutex);
        LockGuard lockRooms(rooms_mutex);

        auto it = rooms.find(roomName);
        if (it == rooms.end()) return members;

        Room& room = it->second;
        auto pos = std::find(room.members.begin(), room.members.end(), clientSocket);
        if (pos == room.members.end()) return members;

        size_t index = pos - room.members.begin();
        members = room.members;
        room.members.erase(pos);
        room.nicknames.erase(room.nicknames.begin() + index);
        room.mutedUntil.erase(clientSocket);
        leaveMsg = "LEAVE|" + clientNicknames[clientSocket] + " left the room\n";

        if (room.members.empty() && room.name != LOBBY_NAME) {
            deleteRoomFromDB(roomName);
            rooms.erase(it);
            Logger::instance().log("Room " + roomName + " deleted (empty)");
        }
        else {
            removeMemberFromRoomDB(clientUserIds[clientSocket], roomName);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] removeFromRoom exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[ERROR] removeFromRoom unknown exception" << std::endl;
    }
    return members;
}

void addToRoom(SOCKET clientSocket, const std::string& roomName) {
    try {
        LockGuard lockClients(clients_mutex);
        LockGuard lockRooms(rooms_mutex);

        auto it = rooms.find(roomName);
        if (it == rooms.end()) {
            std::cerr << "[ERROR] addToRoom: room not found: " << roomName << std::endl;
            return;
        }
        Room& room = it->second;
        room.members.push_back(clientSocket);
        room.nicknames.push_back(clientNicknames[clientSocket]);
        clientRooms[clientSocket] = roomName;
        addMemberToRoomDB(clientUserIds[clientSocket], roomName);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] addToRoom exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[ERROR] addToRoom unknown exception" << std::endl;
    }
}

void handleCreateRoom(SOCKET clientSocket, const std::string& params) {
    std::cout << "[DEBUG] handleCreateRoom called, params=" << params << std::endl;
    std::istringstream ss(params);
    std::string roomName, publicStr, password;
    std::getline(ss, roomName, '|');
    std::getline(ss, publicStr, '|');
    std::getline(ss, password, '|');
    bool isPublic = (publicStr == "1");

    if (roomName.empty()) {
        std::cout << "[DEBUG] room name empty" << std::endl;
        send(clientSocket, "CREATE_FAIL|Room name cannot be empty\n", 38, 0);
        return;
    }

    if (roomName.find_first_of("\\/\"'`;") != std::string::npos) {
        send(clientSocket, "CREATE_FAIL|Room name contains invalid characters\n", 50, 0);
        return;
    }

    try {
        LockGuard lockClients(clients_mutex);
        LockGuard lockRooms(rooms_mutex);
        if (rooms.find(roomName) != rooms.end()) {
            std::cout << "[DEBUG] room already exists" << std::endl;
            send(clientSocket, "CREATE_FAIL|Room already exists\n", 32, 0);
            return;
        }

        Room newRoom;
        newRoom.name = roomName;
        newRoom.isPublic = isPublic;
        newRoom.password = password;
        newRoom.ownerId = clientUserIds[clientSocket];
        newRoom.members.push_back(clientSocket);
        newRoom.nicknames.push_back(clientNicknames[clientSocket]);
        rooms[roomName] = newRoom;
        clientRooms[clientSocket] = roomName;

        createRoomInDB(roomName, isPublic, password, clientUserIds[clientSocket]);
        addMemberToRoomDB(clientUserIds[clientSocket], roomName);

        Logger::instance().log("User " + clientNicknames[clientSocket] + " created room " + roomName);
        std::cout << "[DEBUG] room created successfully" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] handleCreateRoom exception: " << e.what() << std::endl;
        std::string errMsg = "CREATE_FAIL|Server error: " + std::string(e.what()) + "\n";
        send(clientSocket, errMsg.c_str(), (int)errMsg.size(), 0);
        return;
    }
    catch (...) {
        std::cerr << "[ERROR] handleCreateRoom unknown exception" << std::endl;
        send(clientSocket, "CREATE_FAIL|Internal server error\n", 35, 0);
        return;
    }

    std::string msg = "CREATE_OK|" + roomName + "\n";
    send(clientSocket, msg.c_str(), (int)msg.size(), 0);
    broadcastRoomList();
    broadcastMemberList(roomName);
}

void handleJoinRoom(SOCKET clientSocket, const std::string& params) {
    std::istringstream ss(params);
    std::string roomName, password;
    std::getline(ss, roomName, '|');
    std::getline(ss, password, '|');

    std::string leaveBroadcast;
    std::vector<SOCKET> leaveMembers;
    std::string joinBroadcast;
    std::vector<SOCKET> joinMembers;
    std::string nickname;
    int userId;
    std::string oldRoom;

    {
        LockGuard lockClients(clients_mutex);
        nickname = clientNicknames[clientSocket];
        userId = clientUserIds[clientSocket];
        oldRoom = clientRooms[clientSocket];
    }

    try {
        LockGuard lockRooms(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it == rooms.end()) {
            send(clientSocket, "JOIN_FAIL|Room not found\n", 25, 0);
            return;
        }
        Room& room = it->second;
        if (!room.isPublic && room.password != password) {
            send(clientSocket, "JOIN_FAIL|Wrong password\n", 25, 0);
            return;
        }

        if (!oldRoom.empty() && oldRoom != LOBBY_NAME) {
            auto oldIt = rooms.find(oldRoom);
            if (oldIt != rooms.end()) {
                Room& oldRoomObj = oldIt->second;
                auto pos = std::find(oldRoomObj.members.begin(), oldRoomObj.members.end(), clientSocket);
                if (pos != oldRoomObj.members.end()) {
                    size_t idx = pos - oldRoomObj.members.begin();
                    leaveMembers = oldRoomObj.members;
                    oldRoomObj.members.erase(pos);
                    oldRoomObj.nicknames.erase(oldRoomObj.nicknames.begin() + idx);
                    oldRoomObj.mutedUntil.erase(clientSocket);
                    leaveBroadcast = "LEAVE|" + nickname + " left the room\n";
                    if (oldRoomObj.members.empty() && oldRoomObj.name != LOBBY_NAME) {
                        deleteRoomFromDB(oldRoom);
                        rooms.erase(oldIt);
                    }
                    else {
                        removeMemberFromRoomDB(userId, oldRoom);
                    }
                }
            }
        }

        room.members.push_back(clientSocket);
        room.nicknames.push_back(nickname);
        joinBroadcast = "NICK|" + nickname + " joined the room\n";
        joinMembers = room.members;

        {
            LockGuard lockClients(clients_mutex);
            clientRooms[clientSocket] = roomName;
        }
        addMemberToRoomDB(userId, roomName);

        bool isOwner = (userId == room.ownerId);
        std::string joinOk = "JOIN_OK|" + roomName + "|" + (isOwner ? "1" : "0") + "\n";
        send(clientSocket, joinOk.c_str(), (int)joinOk.size(), 0);

        Logger::instance().log("User " + nickname + " joined room " + roomName);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] handleJoinRoom exception: " << e.what() << std::endl;
        send(clientSocket, "JOIN_FAIL|Internal server error\n", 32, 0);
        return;
    }

    if (!leaveBroadcast.empty()) {
        for (SOCKET member : leaveMembers) {
            if (member != clientSocket) send(member, leaveBroadcast.c_str(), (int)leaveBroadcast.size(), 0);
        }
        if (!oldRoom.empty() && oldRoom != LOBBY_NAME) {
            broadcastMemberList(oldRoom);
        }
    }
    for (SOCKET member : joinMembers) {
        if (member != clientSocket) send(member, joinBroadcast.c_str(), (int)joinBroadcast.size(), 0);
    }
    broadcastRoomList();
    broadcastMemberList(roomName);
}

void handleDismissRoom(SOCKET clientSocket, const std::string& roomName) {
    std::vector<SOCKET> members;
    int ownerId;
    std::string adminNick;
    {
        LockGuard lockClients(clients_mutex);
        ownerId = clientUserIds[clientSocket];
        adminNick = clientNicknames[clientSocket];
    }
    LockGuard lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        send(clientSocket, "DISMISS_FAIL|Room not found\n", 28, 0);
        return;
    }
    Room& room = it->second;
    if (ownerId != room.ownerId) {
        send(clientSocket, "DISMISS_FAIL|Only owner can dismiss room\n", 41, 0);
        return;
    }
    members = room.members;
    deleteRoomFromDB(roomName);
    rooms.erase(it);

    {
        LockGuard lockClients(clients_mutex);
        for (SOCKET member : members) {
            clientRooms[member] = "Lobby";
        }
    }

    std::string dismissMsg = "DISMISS|Room has been dismissed by owner\n";
    std::string lobbyMsg = "JOIN_OK|Lobby|0\n";
    for (SOCKET member : members) {
        send(member, dismissMsg.c_str(), (int)dismissMsg.size(), 0);
        send(member, lobbyMsg.c_str(), (int)lobbyMsg.size(), 0);
    }
    broadcastRoomList();
    Logger::instance().log("Owner " + adminNick + " dismissed room " + roomName);
    send(clientSocket, "DISMISS_OK\n", 11, 0);
}

void handleLeaveRoom(SOCKET clientSocket) {
    std::string roomName = clientRooms[clientSocket];
    if (roomName.empty()) return;

    {
        LockGuard lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end() && clientUserIds[clientSocket] == it->second.ownerId) {
            handleDismissRoom(clientSocket, roomName);
            return;
        }
    }

    std::string leaveMsg;
    std::vector<SOCKET> members = removeFromRoom(clientSocket, roomName, leaveMsg);

    send(clientSocket, "LEAVE_ROOM_OK\n", 14, 0);
    for (SOCKET member : members) {
        if (member != clientSocket) send(member, leaveMsg.c_str(), (int)leaveMsg.size(), 0);
    }
    broadcastRoomList();
    
    LockGuard lockRooms(rooms_mutex);
    if (rooms.find(roomName) != rooms.end()) {
        broadcastMemberList(roomName);
    }
}

void handleListRooms(SOCKET clientSocket) {
    LockGuard lock(rooms_mutex);
    std::string roomList;
    for (const auto& pair : rooms) {
        const Room& room = pair.second;
        if (room.name != LOBBY_NAME) {
            roomList += room.name + "|" + (room.isPublic ? "Public" : "Private") + "|" + std::to_string(room.members.size()) + ";";
        }
    }
    if (!roomList.empty()) roomList.pop_back();
    std::string response = "ROOM_LIST|" + roomList + "\n";
    send(clientSocket, response.c_str(), (int)response.size(), 0);
    std::cout << "[ROOM] Sent room list: " << response << std::endl;
}

void handleMessage(SOCKET clientSocket, const std::string& content) {
    std::string roomName;
    std::string nickname;
    int userId;
    {
        LockGuard lock(clients_mutex);
        roomName = clientRooms[clientSocket];
        nickname = clientNicknames[clientSocket];
        userId = clientUserIds[clientSocket];
    }
    if (roomName.empty()) {
        send(clientSocket, "ERROR|Not in a room\n", 20, 0);
        return;
    }
    if (roomName == LOBBY_NAME) {
        send(clientSocket, "ERROR|Lobby does not support chat, please join another room\n", 60, 0);
        return;
    }

    {
        LockGuard lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) {
            auto& mutedUntil = it->second.mutedUntil;
            auto found = mutedUntil.find(clientSocket);
            if (found != mutedUntil.end()) {
                if (found->second > time(nullptr)) {
                    send(clientSocket, "ERROR|You are muted, cannot send message\n", 41, 0);
                    return;
                }
                else {
                    mutedUntil.erase(found);
                }
            }
        }
    }

    int roomId = getRoomIdByName(roomName);
    if (roomId != -1) {
        MYSQL* conn = connectDB();
        if (conn) {
            std::string escContent = escapeString(conn, content);
            std::string query = "INSERT INTO messages (room_id, user_id, content) VALUES (" +
                std::to_string(roomId) + "," + std::to_string(userId) + ",'" + escContent + "')";
            if (mysql_query(conn, query.c_str())) {
                std::cerr << "[DB] Insert message failed: " << mysql_error(conn) << std::endl;
            }
            mysql_close(conn);
        }
    }

    std::string displayNickname = nickname;
    {
        LockGuard lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end() && it->second.ownerId == userId) {
            displayNickname += "[OWNER]";
        }
    }
    
    std::string msg = "MSG|" + displayNickname + ": " + content + "\n";
    std::vector<SOCKET> members;
    {
        LockGuard lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) members = it->second.members;
    }
    std::cout << "[MSG] Broadcasting to room " << roomName << ", member count: " << members.size() << std::endl;
    for (SOCKET member : members) {
        if (member != clientSocket) {
            int sent = send(member, msg.c_str(), (int)msg.size(), 0);
            std::cout << "[MSG] Sent to member socket=" << member << ", result=" << sent << std::endl;
        }
    }
}

void handleKick(SOCKET adminSock, const std::string& roomName, const std::string& targetNick) {
    std::cout << "[DEBUG] handleKick called, room=" << roomName << ", target=" << targetNick << std::endl;
    int adminId;
    std::string adminNick;
    {
        LockGuard lock(clients_mutex);
        adminId = clientUserIds[adminSock];
        adminNick = clientNicknames[adminSock];
    }
    LockGuard lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        std::cout << "[DEBUG] room not found" << std::endl;
        send(adminSock, "KICK_FAIL|Room not found\n", 25, 0);
        return;
    }
    Room& room = it->second;
    if (adminId != room.ownerId) {
        std::cout << "[DEBUG] not owner" << std::endl;
        send(adminSock, "KICK_FAIL|Only owner can kick\n", 30, 0);
        return;
    }
    if (targetNick == adminNick) {
        std::cout << "[DEBUG] can't kick self, sending KICK_FAIL" << std::endl;
        send(adminSock, "KICK_FAIL|Cannot kick yourself\n", 31, 0);
        return;
    }
    SOCKET targetSock = INVALID_SOCKET;
    int idx = -1;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        if (room.nicknames[i] == targetNick) {
            targetSock = room.members[i];
            idx = (int)i;
            break;
        }
    }
    if (targetSock == INVALID_SOCKET) {
        std::cout << "[DEBUG] target not in room" << std::endl;
        send(adminSock, "KICK_FAIL|Target not in room\n", 29, 0);
        return;
    }
    std::cout << "[DEBUG] target found, socket=" << targetSock << std::endl;

    std::string kickMsg = "KICK|You have been kicked by owner\n";
    send(targetSock, kickMsg.c_str(), (int)kickMsg.size(), 0);

    room.members.erase(room.members.begin() + idx);
    room.nicknames.erase(room.nicknames.begin() + idx);
    room.mutedUntil.erase(targetSock);
    removeMemberFromRoomDB(clientUserIds[targetSock], roomName);

    {
        LockGuard lockClients(clients_mutex);
        clientRooms[targetSock] = "";
    }
    std::string notify = "NICK|" + targetNick + " was kicked from the room\n";
    for (SOCKET member : room.members) {
        send(member, notify.c_str(), (int)notify.size(), 0);
    }
    send(adminSock, "KICK_OK\n", 8, 0);
    broadcastRoomList();
    broadcastOnlineCount();
    broadcastMemberList(roomName);
    Logger::instance().log("Owner " + adminNick + " kicked " + targetNick + " from room " + roomName);
    std::cout << "[DEBUG] kick success" << std::endl;
}

void handleMute(SOCKET adminSock, const std::string& roomName, const std::string& targetNick, int minutes) {
    std::cout << "[DEBUG] handleMute called, room=" << roomName << ", target=" << targetNick << std::endl;
    int adminId;
    std::string adminNick;
    {
        LockGuard lock(clients_mutex);
        adminId = clientUserIds[adminSock];
        adminNick = clientNicknames[adminSock];
    }
    LockGuard lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        std::cout << "[DEBUG] room not found" << std::endl;
        send(adminSock, "MUTE_FAIL|Room not found\n", 25, 0);
        return;
    }
    Room& room = it->second;
    if (adminId != room.ownerId) {
        std::cout << "[DEBUG] not owner" << std::endl;
        send(adminSock, "MUTE_FAIL|Only owner can mute\n", 30, 0);
        return;
    }
    if (targetNick == adminNick) {
        std::cout << "[DEBUG] can't mute self, sending MUTE_FAIL" << std::endl;
        send(adminSock, "MUTE_FAIL|Cannot mute yourself\n", 31, 0);
        return;
    }
    SOCKET targetSock = INVALID_SOCKET;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        if (room.nicknames[i] == targetNick) {
            targetSock = room.members[i];
            break;
        }
    }
    if (targetSock == INVALID_SOCKET) {
        send(adminSock, "MUTE_FAIL|Target not in room\n", 29, 0);
        return;
    }
    time_t until = time(nullptr) + minutes * 60;
    room.mutedUntil[targetSock] = until;
    std::string msg = "MUTE|You have been muted for " + std::to_string(minutes) + " minutes\n";
    send(targetSock, msg.c_str(), (int)msg.size(), 0);
    std::string muteOk = "MUTE_OK|Muted " + targetNick + " for " + std::to_string(minutes) + " minutes\n";
    send(adminSock, muteOk.c_str(), (int)muteOk.size(), 0);
    
    std::string notify = "NICK|" + targetNick + " was muted for " + std::to_string(minutes) + " minutes\n";
    for (SOCKET member : room.members) {
        if (member != adminSock && member != targetSock) {
            send(member, notify.c_str(), (int)notify.size(), 0);
        }
    }
    
    Logger::instance().log("Owner " + adminNick + " muted " + targetNick + " for " + std::to_string(minutes) + " minutes");
}

void handleGetHistory(SOCKET clientSocket, const std::string& roomName) {
    int roomId = getRoomIdByName(roomName);
    if (roomId == -1) {
        send(clientSocket, "HISTORY_ERR|Room not found\n", 27, 0);
        return;
    }
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string query = "SELECT user_id, content, sent_at, id FROM messages WHERE room_id=" + std::to_string(roomId) +
        " ORDER BY sent_at ASC, id ASC LIMIT 50";
    if (mysql_query(conn, query.c_str())) {
        mysql_close(conn);
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) { mysql_close(conn); return; }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        int userId = (row[0] == nullptr) ? 0 : atoi(row[0]);
        std::string content = row[1] ? row[1] : "";
        std::string sent_at = row[2] ? row[2] : "";
        std::string nickname = getNicknameById(userId);

        LockGuard lockRooms(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end() && userId == it->second.ownerId) {
            nickname += "[OWNER]";
        }

        std::string historyLine = "MSG|" + nickname + ": " + content + "\n";
        send(clientSocket, historyLine.c_str(), (int)historyLine.size(), 0);
    }
    mysql_free_result(res);
    mysql_close(conn);

    send(clientSocket, "HISTORY_END\n", 12, 0);
}

std::string httpPost(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
    std::string response;
    
    std::regex urlPattern(R"(https?://([^/]+)(:\d+)?(/.*)?)");
    std::smatch match;
    if (!std::regex_match(url, match, urlPattern)) {
        std::cerr << "[AI] Invalid URL: " << url << std::endl;
        return "";
    }
    
    std::string host = match[1].str();
    std::string portStr = match.size() > 2 && !match[2].str().empty() ? match[2].str().substr(1) : "";
    std::string path = match.size() > 3 ? match[3].str() : "/";
    bool isHttps = url.substr(0, 5) == "https";
    int port = isHttps ? 443 : 80;
    if (!portStr.empty()) {
        port = std::stoi(portStr);
    }

    HINTERNET hSession = WinHttpOpen(L"ChatServer AI Client", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        std::cerr << "[AI] Failed to open WinHTTP session" << std::endl;
        return "";
    }

    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hConnect = WinHttpConnect(hSession, std::wstring(host.begin(), host.end()).c_str(), port, 0);
    if (!hConnect) {
        std::cerr << "[AI] Failed to connect to host: " << host << std::endl;
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", std::wstring(path.begin(), path.end()).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) {
        std::cerr << "[AI] Failed to open request" << std::endl;
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    std::wstring acceptEncoding = L"Accept-Encoding: gzip, deflate";
    WinHttpAddRequestHeaders(hRequest, acceptEncoding.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    for (const auto& header : headers) {
        std::wstring wHeader = std::wstring(header.first.begin(), header.first.end()) + L": " + std::wstring(header.second.begin(), header.second.end());
        WinHttpAddRequestHeaders(hRequest, wHeader.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0)) {
        std::cerr << "[AI] Failed to send request" << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        std::cerr << "[AI] Failed to receive response" << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &statusCode, &statusCodeSize, NULL);
    
    if (statusCode != 200) {
        DWORD bytesRead = 0;
        char errorBuffer[4096];
        do {
            if (!WinHttpReadData(hRequest, errorBuffer, sizeof(errorBuffer) - 1, &bytesRead)) {
                break;
            }
            if (bytesRead > 0) {
                errorBuffer[bytesRead] = '\0';
                std::cerr << "[AI] HTTP error response: " << errorBuffer << std::endl;
            }
        } while (bytesRead > 0);
        
        std::cerr << "[AI] HTTP error: " << statusCode << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD bytesRead = 0;
    char buffer[4096];
    do {
        if (!WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead)) {
            break;
        }
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
    } while (bytesRead > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

void handleAIRequest(SOCKET clientSocket, const std::string& question) {
    std::cout << "[AI] Processing AI request: " << question << std::endl;

    std::string nickname;
    std::string roomName;
    {
        LockGuard lock(clients_mutex);
        nickname = clientNicknames[clientSocket];
        roomName = clientRooms[clientSocket];
    }

    try {
        nlohmann::json requestBody;
        requestBody["model"] = g_config.llm.model;
        requestBody["messages"] = nlohmann::json::array();
        requestBody["stream"] = false;

        int contextLength = g_config.llm.context_length;
        if (contextLength <= 0) {
            contextLength = 10;
        }

        std::string ragContext = "";
        if (g_config.rag.enabled && g_ragFTS != nullptr && g_config.rag.mode == "fts5") {
            int roomId = getRoomIdByName(roomName);
            if (roomId != -1) {
                std::vector<std::string> similarMessages = g_ragFTS->searchSimilar(question, roomId, g_config.rag.top_k);
                if (!similarMessages.empty()) {
                    ragContext = "相关历史消息：\n";
                    for (size_t i = 0; i < similarMessages.size(); ++i) {
                        ragContext += std::to_string(i + 1) + ". " + similarMessages[i] + "\n";
                    }
                }
            }
        }

        {
            LockGuard lock(rooms_mutex);
            auto it = rooms.find(roomName);
            if (it != rooms.end()) {
                const auto& history = it->second.aiChatHistory;
                int startIdx = std::max(0, (int)history.size() - contextLength);
                for (size_t i = startIdx; i < history.size(); ++i) {
                    const auto& record = history[i];
                    std::string userMsg = std::get<0>(record) + ": " + std::get<1>(record);
                    requestBody["messages"].push_back({
                        {"role", "user"},
                        {"content", userMsg}
                    });
                    requestBody["messages"].push_back({
                        {"role", "assistant"},
                        {"content", std::get<2>(record)}
                    });
                }
            }
        }

        std::string currentUserMsg = nickname + ": " + question;
        if (!ragContext.empty()) {
            currentUserMsg = ragContext + "\n" + currentUserMsg;
        }
        requestBody["messages"].push_back({
            {"role", "user"},
            {"content", currentUserMsg}
        });

        std::string body = requestBody.dump();
        std::cout << "[AI] Request body: " << body << std::endl;

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";
        headers["Authorization"] = "Bearer " + g_config.llm.api_key;

        std::cout << "[AI] Sending request to: " << g_config.llm.api_url << std::endl;
        std::string response = httpPost(g_config.llm.api_url, body, headers);

        std::cout << "[AI] Response: " << response << std::endl;

        if (response.empty()) {
            std::cerr << "[AI] Empty response from API" << std::endl;
            send(clientSocket, "AI_REPLY|抱歉，AI 服务暂时不可用。\n", 42, 0);
            return;
        }

        nlohmann::json responseJson = nlohmann::json::parse(response);

        if (responseJson.contains("choices") && responseJson["choices"].is_array() && responseJson["choices"].size() > 0) {
            std::string reply = responseJson["choices"][0]["message"]["content"];
            std::cout << "[AI] AI reply: " << reply << std::endl;

            {
                LockGuard lock(rooms_mutex);
                auto it = rooms.find(roomName);
                if (it != rooms.end()) {
                    auto& history = it->second.aiChatHistory;
                    history.push_back(std::make_tuple(nickname, question, reply));
                    while (history.size() > (size_t)contextLength) {
                        history.pop_front();
                    }
                }
            }

            std::string escapedReply = reply;
            size_t pos = 0;
            while ((pos = escapedReply.find("\n", pos)) != std::string::npos) {
                escapedReply.replace(pos, 1, "<br>");
                pos += 4;
            }
            
            std::string aiMsg = "MSG|[AI]: " + escapedReply + "\n";

            if (!roomName.empty()) {
                std::vector<SOCKET> members;
                {
                    LockGuard lock(rooms_mutex);
                    auto it = rooms.find(roomName);
                    if (it != rooms.end()) {
                        members = it->second.members;
                    }
                }

                int roomId = getRoomIdByName(roomName);
                std::cout << "[AI] Room ID: " << roomId << std::endl;
                if (roomId != -1) {
                    MYSQL* conn = connectDB();
                    if (conn) {
                        std::string escContent = escapeString(conn, reply);
                        std::string query = "INSERT INTO messages (room_id, user_id, content) VALUES (" +
                            std::to_string(roomId) + ", NULL, '" + escContent + "')";
                        std::cout << "[AI] Saving AI reply to DB: " << query << std::endl;
                        if (mysql_query(conn, query.c_str())) {
                            std::cerr << "[DB] Insert AI message failed: " << mysql_error(conn) << std::endl;
                        } else {
                            std::cout << "[AI] AI reply saved to database successfully" << std::endl;
                        }
                        mysql_close(conn);
                    } else {
                        std::cerr << "[AI] Failed to connect to database" << std::endl;
                    }
                } else {
                    std::cerr << "[AI] Room not found: " << roomName << std::endl;
                }

                for (SOCKET member : members) {
                    send(member, aiMsg.c_str(), (int)aiMsg.size(), 0);
                }
            }
        }
        else {
            std::cerr << "[AI] Invalid response format: " << response << std::endl;
            send(clientSocket, "AI_REPLY|抱歉，AI 服务暂时不可用。\n", 42, 0);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[AI] Exception: " << e.what() << std::endl;
        send(clientSocket, "AI_REPLY|抱歉，AI 服务暂时不可用。\n", 42, 0);
    }
}

void processClientMessage(SOCKET clientSocket, const std::string& line) {
    size_t sep = line.find('|');
    if (sep == std::string::npos) return;
    std::string type = line.substr(0, sep);
    std::string data = line.substr(sep + 1);

    if (type == "MSG") {
        if (data.substr(0, 4) == "@AI ") {
            std::string question = data.substr(4);
            handleMessage(clientSocket, data);
            std::async(std::launch::async, handleAIRequest, clientSocket, question);
        } else {
            handleMessage(clientSocket, data);
        }
    }
    else if (type == "AI_CHAT") {
        std::async(std::launch::async, handleAIRequest, clientSocket, data);
    }
    else if (type == "CREATE") handleCreateRoom(clientSocket, data);
    else if (type == "JOIN") handleJoinRoom(clientSocket, data);
    else if (type == "LEAVE_ROOM") handleLeaveRoom(clientSocket);
    else if (type == "LIST_ROOMS") handleListRooms(clientSocket);
    else if (type == "GET_MEMBERS") handleGetMembers(clientSocket, data);
    else if (type == "PING") {
        LockGuard lock(clients_mutex);
        clientLastHeartbeat[clientSocket] = std::chrono::steady_clock::now();
        send(clientSocket, "PONG\n", 5, 0);
    }
    else if (type == "GET_HISTORY") handleGetHistory(clientSocket, data);
    else if (type == "KICK") {
        size_t sep2 = data.find('|');
        if (sep2 == std::string::npos) return;
        std::string roomName = data.substr(0, sep2);
        std::string targetNick = data.substr(sep2 + 1);
        handleKick(clientSocket, roomName, targetNick);
    }
    else if (type == "MUTE") {
        std::vector<std::string> parts;
        size_t start = 0, end;
        while ((end = data.find('|', start)) != std::string::npos) {
            parts.push_back(data.substr(start, end - start));
            start = end + 1;
        }
        parts.push_back(data.substr(start));
        if (parts.size() < 3) return;
        std::string roomName = parts[0];
        std::string targetNick = parts[1];
        int minutes = std::stoi(parts[2]);
        handleMute(clientSocket, roomName, targetNick, minutes);
    }
    else if (type == "DISMISS_ROOM") {
        handleDismissRoom(clientSocket, data);
    }
}

void handle_client(SOCKET clientSocket) {
    std::string receiveBuffer;
    bool authenticated = false;
    std::string nickname;
    int userId = 0;

    while (!authenticated) {
        char temp[1024];
        int bytesReceived = recv(clientSocket, temp, sizeof(temp), 0);
        if (bytesReceived <= 0) {
            closesocket(clientSocket);
            return;
        }
        receiveBuffer.append(temp, bytesReceived);

        size_t pos;
        while ((pos = receiveBuffer.find('\n')) != std::string::npos) {
            std::string line = receiveBuffer.substr(0, pos);
            receiveBuffer.erase(0, pos + 1);

            size_t sep = line.find('|');
            if (sep == std::string::npos) continue;
            std::string type = line.substr(0, sep);
            std::string data = line.substr(sep + 1);

            if (type == "AUTH") {
                size_t sep2 = data.find('|');
                if (sep2 == std::string::npos) continue;
                std::string username = data.substr(0, sep2);
                std::string password = data.substr(sep2 + 1);
                if (authenticateUser(username, password, userId)) {
                    authenticated = true;
                    nickname = username;
                    std::string okMsg = "AUTH_OK|" + nickname + "\n";
                    send(clientSocket, okMsg.c_str(), (int)okMsg.size(), 0);
                    Logger::instance().log("User " + username + " logged in, ID=" + std::to_string(userId));
                }
                else {
                    send(clientSocket, "AUTH_FAIL\n", 10, 0);
                    closesocket(clientSocket);
                    return;
                }
            }
            else if (type == "REG") {
                size_t sep2 = data.find('|');
                std::string username = data.substr(0, sep2);
                std::string password = data.substr(sep2 + 1);
                if (registerUser(username, password)) {
                    send(clientSocket, "REG_OK\n", 7, 0);
                    Logger::instance().log("New user registered: " + username);
                    continue;
                }
                else {
                    send(clientSocket, "REG_FAIL\n", 9, 0);
                    closesocket(clientSocket);
                    return;
                }
            }
            if (authenticated) break;
        }
    }

    try {
        std::cout << "[INFO] User " << nickname << " authenticated, joining system" << std::endl;

        {
            LockGuard lock(clients_mutex);
            clients.push_back(clientSocket);
            clientNicknames[clientSocket] = nickname;
            clientUserIds[clientSocket] = userId;
            clientLastHeartbeat[clientSocket] = std::chrono::steady_clock::now();
        }

        {
            LockGuard lockRooms(rooms_mutex);
            if (rooms.find(LOBBY_NAME) == rooms.end()) {
                Room hall;
                hall.name = LOBBY_NAME;
                hall.isPublic = true;
                hall.password = "";
                hall.ownerId = 0;
                rooms[LOBBY_NAME] = hall;
                createRoomInDB(LOBBY_NAME, true, "", 0);
                std::cout << "[INFO] Created default lobby room" << std::endl;
            }
        }
        addToRoom(clientSocket, LOBBY_NAME);
        broadcastOnlineCount();
        broadcastRoomList();
        handleListRooms(clientSocket);

        while (true) {
            char buffer[1024];
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (bytesReceived <= 0) {
                std::cout << "[INFO] User " << nickname << " disconnected" << std::endl;
                break;
            }

            receiveBuffer.append(buffer, bytesReceived);
            size_t pos;
            while ((pos = receiveBuffer.find('\n')) != std::string::npos) {
                std::string line = receiveBuffer.substr(0, pos);
                receiveBuffer.erase(0, pos + 1);
                processClientMessage(clientSocket, line);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[EXCEPTION] Exception handling client " << nickname << ": " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[EXCEPTION] Unknown exception, client " << nickname << " disconnected" << std::endl;
    }

    cleanupClient(clientSocket);
    Logger::instance().log("User " + nickname + " disconnected");
}

void startServer() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[ERROR] WSAStartup failed" << std::endl;
        return;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "[ERROR] socket creation failed" << std::endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_config.server.port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] bind failed" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[ERROR] listen failed" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    std::cout << "[INFO] Server started on port " << g_config.server.port << "..." << std::endl;

    std::cout << "[AI] AI helper initialized successfully" << std::endl;
    std::cout << "[AI] API URL: " << g_config.llm.api_url << std::endl;
    std::cout << "[AI] Model: " << g_config.llm.model << std::endl;
    std::cout << "[AI] API Key: " << (g_config.llm.api_key.empty() ? "empty" : "set") << std::endl;

    if (g_config.rag.enabled && g_config.rag.mode == "fts5") {
        g_ragFTS = new RAGFTS("rag_index.db");
        if (g_ragFTS->init()) {
            std::cout << "[RAG] FTS5 RAG initialized successfully" << std::endl;
        } else {
            std::cerr << "[RAG] Failed to initialize FTS5 RAG" << std::endl;
            delete g_ragFTS;
            g_ragFTS = nullptr;
        }
    }

    loadRoomsFromDB();
    {
        LockGuard lock(rooms_mutex);
        if (rooms.find(LOBBY_NAME) == rooms.end()) {
            Room hall;
            hall.name = LOBBY_NAME;
            hall.isPublic = true;
            hall.password = "";
            hall.ownerId = 0;
            rooms[LOBBY_NAME] = hall;
            createRoomInDB(LOBBY_NAME, true, "", 0);
            std::cout << "[INFO] Created default lobby room" << std::endl;
        }
    }

    std::thread heartbeatChecker([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(g_config.heartbeat.check_interval_sec));
            auto now = std::chrono::steady_clock::now();
            LockGuard lock(clients_mutex);
            std::vector<SOCKET> toKill;
            for (auto it = clientLastHeartbeat.begin(); it != clientLastHeartbeat.end(); ++it) {
                SOCKET sock = it->first;
                auto last = it->second;
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
                if (elapsed > g_config.heartbeat.timeout_sec) {
                    std::cout << "[HEARTBEAT] Client " << sock << " timeout, disconnecting" << std::endl;
                    toKill.push_back(sock);
                }
            }
            for (SOCKET sock : toKill) {
                cleanupClient(sock);
            }
        }
        });
    heartbeatChecker.detach();

    if (g_config.rag.enabled && g_config.rag.mode == "fts5" && g_ragFTS != nullptr) {
        std::thread ragSyncThread([]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(g_config.rag.sync_interval_sec));
                
                if (g_ragFTS == nullptr) break;
                
                int lastSyncedId = g_ragFTS->getLastSyncedId();
                int newLastSyncedId = lastSyncedId;
                
                if (g_ragFTS->syncMessagesFromMySQL(lastSyncedId, newLastSyncedId)) {
                    g_ragFTS->setLastSyncedId(newLastSyncedId);
                }
            }
        });
        ragSyncThread.detach();
        std::cout << "[RAG] RAG sync thread started, interval: " << g_config.rag.sync_interval_sec << "s" << std::endl;
    }

    while (true) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "[ERROR] accept failed" << std::endl;
            continue;
        }
        std::cout << "[INFO] New client connected" << std::endl;
        std::thread(handle_client, clientSocket).detach();
    }

    closesocket(serverSocket);
    WSACleanup();
}