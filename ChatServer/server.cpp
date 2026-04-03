#include "utils.h"
#include "database.h"
#include "config.h"
#include "logger.h"
#include <thread>
#include <algorithm>
#include <sstream>
#include <mutex>
#include <iostream>
#include <vector>
#include <map>
#include <stdexcept>
#include <chrono>

// 全局常量定义
const std::string LOBBY_NAME = "Lobby";

// 全局变量定义
std::vector<SOCKET> clients;
std::map<SOCKET, std::string> clientNicknames;
std::map<SOCKET, int> clientUserIds;
std::map<SOCKET, std::string> clientRooms;
std::map<std::string, Room> rooms;
std::mutex clients_mutex;
std::mutex rooms_mutex;
std::map<SOCKET, std::chrono::steady_clock::time_point> clientLastHeartbeat;

extern AppConfig g_config;

// 辅助函数：根据 socket 获取昵称（需加锁）
std::string getNicknameBySocket(SOCKET sock) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = clientNicknames.find(sock);
    if (it != clientNicknames.end()) return it->second;
    return "";
}

// 广播房间列表
void broadcastRoomList() {
    std::string roomList;
    {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        for (const auto& pair : rooms) {
            const Room& room = pair.second;
            // 跳过大厅，只广播实际创建的房间
            if (room.name != LOBBY_NAME) {
                roomList += room.name + "|" + (room.isPublic ? "Public" : "Private") + "|" + std::to_string(room.members.size()) + ";";
            }
        }
    }
    if (!roomList.empty()) roomList.pop_back();
    std::string response = "ROOM_LIST|" + roomList + "\n";

    std::lock_guard<std::mutex> lock(clients_mutex);
    for (SOCKET client : clients) {
        send(client, response.c_str(), response.size(), 0);
    }
    std::cout << "[BROADCAST] 房间列表广播: " << response << std::endl;
}

// 广播在线人数
void broadcastOnlineCount() {
    int count;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        count = clients.size();
    }
    std::string msg = "ONLINE_COUNT|" + std::to_string(count) + "\n";
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (SOCKET client : clients) {
        send(client, msg.c_str(), msg.size(), 0);
    }
    std::cout << "[BROADCAST] 在线人数: " << count << std::endl;
}

// 清理客户端（断开连接时调用）
void cleanupClient(SOCKET clientSocket) {
    std::string leaveMsg;
    std::vector<SOCKET> members;
    std::string roomName;
    std::string nickname;
    int userId;
    // 先获取客户端信息
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        roomName = clientRooms[clientSocket];
        nickname = clientNicknames[clientSocket];
        userId = clientUserIds[clientSocket];
    }
    if (!roomName.empty()) {
        std::lock_guard<std::mutex> lockRooms(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) {
            Room& room = it->second;
            auto pos = std::find(room.members.begin(), room.members.end(), clientSocket);
            if (pos != room.members.end()) {
                size_t idx = pos - room.members.begin();
                room.members.erase(pos);
                room.nicknames.erase(room.nicknames.begin() + idx);
                room.mutedUntil.erase(clientSocket);
                leaveMsg = "LEAVE|" + nickname + " 离开了房间\n";
                members = room.members;
                if (room.members.empty() && room.name != LOBBY_NAME) {
                    deleteRoomFromDB(room.name);
                    rooms.erase(it);
                    Logger::instance().log("房间 " + room.name + " 因空被删除");
                }
                else {
                    removeMemberFromRoomDB(userId, roomName);
                }
            }
        }
    }
    // 发送离开消息
    for (SOCKET member : members) {
        if (member != clientSocket) send(member, leaveMsg.c_str(), leaveMsg.size(), 0);
    }
    // 最后从全局容器中移除客户端
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
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

// 安全地从房间移除成员，并返回该房间的成员列表
std::vector<SOCKET> removeFromRoom(SOCKET clientSocket, const std::string& roomName, std::string& leaveMsg) {
    std::vector<SOCKET> members;
    try {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        std::lock_guard<std::mutex> lockRooms(rooms_mutex);

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
        leaveMsg = "LEAVE|" + clientNicknames[clientSocket] + " 离开了房间\n";

        if (room.members.empty() && room.name != LOBBY_NAME) {
            deleteRoomFromDB(roomName);
            rooms.erase(it);
            Logger::instance().log("房间 " + roomName + " 因空被删除");
        }
        else {
            removeMemberFromRoomDB(clientUserIds[clientSocket], roomName);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] removeFromRoom 异常: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[ERROR] removeFromRoom 未知异常" << std::endl;
    }
    return members;
}

void addToRoom(SOCKET clientSocket, const std::string& roomName) {
    try {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        std::lock_guard<std::mutex> lockRooms(rooms_mutex);

        auto it = rooms.find(roomName);
        if (it == rooms.end()) {
            std::cerr << "[ERROR] addToRoom: 房间不存在: " << roomName << std::endl;
            return;
        }
        Room& room = it->second;
        room.members.push_back(clientSocket);
        room.nicknames.push_back(clientNicknames[clientSocket]);
        clientRooms[clientSocket] = roomName;
        addMemberToRoomDB(clientUserIds[clientSocket], roomName);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] addToRoom 异常: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[ERROR] addToRoom 未知异常" << std::endl;
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
        send(clientSocket, "CREATE_FAIL|房间名不能为空\n", 27, 0);
        return;
    }

    // 检查房间名是否包含非法字符（可选项，避免数据库错误）
    if (roomName.find_first_of("\\/\"'`;") != std::string::npos) {
        send(clientSocket, "CREATE_FAIL|房间名包含非法字符\n", 31, 0);
        return;
    }

    try {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        std::lock_guard<std::mutex> lockRooms(rooms_mutex);
        if (rooms.find(roomName) != rooms.end()) {
            std::cout << "[DEBUG] room already exists" << std::endl;
            send(clientSocket, "CREATE_FAIL|房间已存在\n", 24, 0);
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

        // 写入数据库
        createRoomInDB(roomName, isPublic, password, clientUserIds[clientSocket]);
        addMemberToRoomDB(clientUserIds[clientSocket], roomName);

        Logger::instance().log("用户 " + clientNicknames[clientSocket] + " 创建房间 " + roomName);
        std::cout << "[DEBUG] room created successfully" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] handleCreateRoom 异常: " << e.what() << std::endl;
        // 发送具体错误信息给客户端，便于调试
        std::string errMsg = "CREATE_FAIL|服务器错误: " + std::string(e.what()) + "\n";
        send(clientSocket, errMsg.c_str(), errMsg.size(), 0);
        return;
    }
    catch (...) {
        std::cerr << "[ERROR] handleCreateRoom 未知异常" << std::endl;
        send(clientSocket, "CREATE_FAIL|服务器内部错误\n", 28, 0);
        return;
    }

    std::string msg = "CREATE_OK|" + roomName + "\n";
    send(clientSocket, msg.c_str(), msg.size(), 0);
    broadcastRoomList();
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

    try {
        auto it = rooms.find(roomName);
        if (it == rooms.end()) {
            send(clientSocket, "JOIN_FAIL|房间不存在\n", 22, 0);
            return;
        }
        Room& room = it->second;
        if (!room.isPublic && room.password != password) {
            send(clientSocket, "JOIN_FAIL|密码错误\n", 20, 0);
            return;
        }

        // 离开当前房间
        std::string oldRoom = clientRooms[clientSocket];
        if (!oldRoom.empty()) {
            leaveMembers = removeFromRoom(clientSocket, oldRoom, leaveBroadcast);
        }

        // 加入新房间
        room.members.push_back(clientSocket);
        room.nicknames.push_back(clientNicknames[clientSocket]);
        clientRooms[clientSocket] = roomName;
        addMemberToRoomDB(clientUserIds[clientSocket], roomName);
        joinBroadcast = "NICK|" + clientNicknames[clientSocket] + " 加入了房间\n";
        joinMembers = room.members;

        Logger::instance().log("用户 " + clientNicknames[clientSocket] + " 加入房间 " + roomName);

        // 发送成功响应，附带房主标志
        bool isOwner = (clientUserIds[clientSocket] == room.ownerId);
        std::string joinOk = "JOIN_OK|" + roomName + "|" + (isOwner ? "1" : "0") + "\n";
        send(clientSocket, joinOk.c_str(), (int)joinOk.size(), 0);

        if (!leaveBroadcast.empty()) {
            for (SOCKET member : leaveMembers) {
                if (member != clientSocket) send(member, leaveBroadcast.c_str(), leaveBroadcast.size(), 0);
            }
        }
        for (SOCKET member : joinMembers) {
            if (member != clientSocket) send(member, joinBroadcast.c_str(), joinBroadcast.size(), 0);
        }
        broadcastRoomList();
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] handleJoinRoom 异常: " << e.what() << std::endl;
        send(clientSocket, "JOIN_FAIL|服务器内部错误\n", 26, 0);
        return;
    }
}

void handleDismissRoom(SOCKET clientSocket, const std::string& roomName) {
    std::vector<SOCKET> members;
    int ownerId;
    std::string adminNick;
    {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        ownerId = clientUserIds[clientSocket];
        adminNick = clientNicknames[clientSocket];
    }
    std::lock_guard<std::mutex> lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        send(clientSocket, "DISMISS_FAIL|房间不存在\n", 24, 0);
        return;
    }
    Room& room = it->second;
    if (ownerId != room.ownerId) {
        send(clientSocket, "DISMISS_FAIL|只有房主可以解散房间\n", 34, 0);
        return;
    }
    members = room.members;  // 复制成员列表
    deleteRoomFromDB(roomName);
    rooms.erase(it);

    // 更新被踢客户端的房间映射（需要锁 clients_mutex）
    {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        for (SOCKET member : members) {
            clientRooms[member] = "Lobby";
        }
    }

    std::string dismissMsg = "DISMISS|房间已被房主解散\n";
    std::string lobbyMsg = "JOIN_OK|Lobby|0\n";
    for (SOCKET member : members) {
        send(member, dismissMsg.c_str(), dismissMsg.size(), 0);
        send(member, lobbyMsg.c_str(), lobbyMsg.size(), 0);
    }
    broadcastRoomList();
    Logger::instance().log("房主 " + adminNick + " 解散房间 " + roomName);
    send(clientSocket, "DISMISS_OK\n", 11, 0);
}

void handleLeaveRoom(SOCKET clientSocket) {
    std::string roomName = clientRooms[clientSocket];
    if (roomName.empty()) return;

    // 检查是否是房主
    {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end() && clientUserIds[clientSocket] == it->second.ownerId) {
            // 房主离开，解散房间
            handleDismissRoom(clientSocket, roomName);
            return;
        }
    }

    // 普通成员离开逻辑
    std::string leaveMsg;
    std::vector<SOCKET> members = removeFromRoom(clientSocket, roomName, leaveMsg);

    send(clientSocket, "LEAVE_ROOM_OK\n", 14, 0);
    for (SOCKET member : members) {
        if (member != clientSocket) send(member, leaveMsg.c_str(), leaveMsg.size(), 0);
    }
    broadcastRoomList();
}

void handleListRooms(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(rooms_mutex);
    std::string roomList;
    for (const auto& pair : rooms) {
        const Room& room = pair.second;
        // 跳过大厅，只发送实际创建的房间
        if (room.name != LOBBY_NAME) {
            roomList += room.name + "|" + (room.isPublic ? "Public" : "Private") + "|" + std::to_string(room.members.size()) + ";";
        }
    }
    if (!roomList.empty()) roomList.pop_back();
    std::string response = "ROOM_LIST|" + roomList + "\n";
    send(clientSocket, response.c_str(), response.size(), 0);
    std::cout << "[ROOM] 发送房间列表: " << response << std::endl;
}

void handleMessage(SOCKET clientSocket, const std::string& content) {
    std::string roomName;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        roomName = clientRooms[clientSocket];
    }
    if (roomName.empty()) {
        send(clientSocket, "ERROR|未加入房间\n", 18, 0);
        return;
    }
    if (roomName == LOBBY_NAME) {
        send(clientSocket, "ERROR|大厅不支持聊天，请加入其他房间\n", 38, 0);
        return;
    }

    // 检查禁言
    {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) {
            auto& mutedUntil = it->second.mutedUntil;
            auto found = mutedUntil.find(clientSocket);
            if (found != mutedUntil.end()) {
                if (found->second > time(nullptr)) {
                    send(clientSocket, "ERROR|您已被禁言，无法发言\n", 28, 0);
                    return;
                }
                else {
                    mutedUntil.erase(found);
                }
            }
        }
    }

    // 写入数据库（同步）
    int roomId = getRoomIdByName(roomName);
    if (roomId != -1) {
        MYSQL* conn = connectDB();
        if (conn) {
            std::string escContent = escapeString(conn, content);
            std::string query = "INSERT INTO messages (room_id, user_id, content) VALUES (" +
                std::to_string(roomId) + "," + std::to_string(clientUserIds[clientSocket]) + ",'" + escContent + "')";
            if (mysql_query(conn, query.c_str())) {
                std::cerr << "[DB] 插入消息失败: " << mysql_error(conn) << std::endl;
            }
            mysql_close(conn);
        }
    }

    // 广播消息
    std::string msg = "MSG|" + clientNicknames[clientSocket] + ": " + content + "\n";
    std::vector<SOCKET> members;
    {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto it = rooms.find(roomName);
        if (it != rooms.end()) members = it->second.members;
    }
    for (SOCKET member : members) {
        if (member != clientSocket) send(member, msg.c_str(), msg.size(), 0);
    }
}

void handleKick(SOCKET adminSock, const std::string& roomName, const std::string& targetNick) {
    std::cout << "[DEBUG] handleKick called, room=" << roomName << ", target=" << targetNick << std::endl;
    // 先获取 admin 信息，需要锁 clients_mutex
    int adminId;
    std::string adminNick;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        adminId = clientUserIds[adminSock];
        adminNick = clientNicknames[adminSock];
    }
    // 现在锁 rooms_mutex
    std::lock_guard<std::mutex> lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        std::cout << "[DEBUG] room not found" << std::endl;
        send(adminSock, "KICK_FAIL|房间不存在\n", 22, 0);
        return;
    }
    Room& room = it->second;
    if (adminId != room.ownerId) {
        std::cout << "[DEBUG] not owner" << std::endl;
        send(adminSock, "KICK_FAIL|只有房主可以踢人\n", 28, 0);
        return;
    }
    if (targetNick == adminNick) {
        std::cout << "[DEBUG] can't kick self, sending KICK_FAIL" << std::endl;
        send(adminSock, "KICK_FAIL|不能踢自己\n", 22, 0);
        return;
    }
    // 查找目标
    SOCKET targetSock = INVALID_SOCKET;
    int idx = -1;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        if (room.nicknames[i] == targetNick) {
            targetSock = room.members[i];
            idx = i;
            break;
        }
    }
    if (targetSock == INVALID_SOCKET) {
        std::cout << "[DEBUG] target not in room" << std::endl;
        send(adminSock, "KICK_FAIL|目标不在房间内\n", 26, 0);
        return;
    }
    std::cout << "[DEBUG] target found, socket=" << targetSock << std::endl;

    // 执行踢出
    std::string kickMsg = "KICK|您被房主踢出房间\n";
    send(targetSock, kickMsg.c_str(), kickMsg.size(), 0);

    // 移除成员
    room.members.erase(room.members.begin() + idx);
    room.nicknames.erase(room.nicknames.begin() + idx);
    room.mutedUntil.erase(targetSock);
    removeMemberFromRoomDB(clientUserIds[targetSock], roomName);

    // 更新被踢客户端的房间映射
    {
        std::lock_guard<std::mutex> lockClients(clients_mutex);
        clientRooms[targetSock] = "";
    }
    // 通知房间内其他人
    std::string notify = "NICK|" + targetNick + " 被房主踢出了房间\n";
    for (SOCKET member : room.members) {
        send(member, notify.c_str(), notify.size(), 0);
    }
    send(adminSock, "KICK_OK\n", 8, 0);
    broadcastRoomList();
    broadcastOnlineCount();
    Logger::instance().log("房主 " + adminNick + " 踢出 " + targetNick + " 从房间 " + roomName);
    std::cout << "[DEBUG] kick success" << std::endl;
}

void handleMute(SOCKET adminSock, const std::string& roomName, const std::string& targetNick, int minutes) {
    std::cout << "[DEBUG] handleMute called, room=" << roomName << ", target=" << targetNick << std::endl;
    // 先获取 admin 信息，需要锁 clients_mutex
    int adminId;
    std::string adminNick;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        adminId = clientUserIds[adminSock];
        adminNick = clientNicknames[adminSock];
    }
    // 现在锁 rooms_mutex
    std::lock_guard<std::mutex> lockRooms(rooms_mutex);
    auto it = rooms.find(roomName);
    if (it == rooms.end()) {
        std::cout << "[DEBUG] room not found" << std::endl;
        send(adminSock, "MUTE_FAIL|房间不存在\n", 22, 0);
        return;
    }
    Room& room = it->second;
    if (adminId != room.ownerId) {
        std::cout << "[DEBUG] not owner" << std::endl;
        send(adminSock, "MUTE_FAIL|只有房主可以禁言\n", 28, 0);
        return;
    }
    if (targetNick == adminNick) {
        std::cout << "[DEBUG] can't mute self, sending MUTE_FAIL" << std::endl;
        send(adminSock, "MUTE_FAIL|不能禁言自己\n", 22, 0);
        return;
    }
    // 查找目标
    SOCKET targetSock = INVALID_SOCKET;
    for (size_t i = 0; i < room.nicknames.size(); ++i) {
        if (room.nicknames[i] == targetNick) {
            targetSock = room.members[i];
            break;
        }
    }
    if (targetSock == INVALID_SOCKET) {
        send(adminSock, "MUTE_FAIL|目标不在房间内\n", 26, 0);
        return;
    }
    time_t until = time(nullptr) + minutes * 60;
    room.mutedUntil[targetSock] = until;
    std::string msg = "MUTE|您被禁言 " + std::to_string(minutes) + " 分钟\n";
    send(targetSock, msg.c_str(), msg.size(), 0);
    std::string muteOk = "MUTE_OK|已禁言 " + targetNick + " " + std::to_string(minutes) + " 分钟\n";
    send(adminSock, muteOk.c_str(), (int)muteOk.size(), 0);
    Logger::instance().log("房主 " + adminNick + " 禁言 " + targetNick + " " + std::to_string(minutes) + " 分钟");
}

void handleGetHistory(SOCKET clientSocket, const std::string& roomName) {
    int roomId = getRoomIdByName(roomName);
    if (roomId == -1) {
        send(clientSocket, "HISTORY_ERR|房间不存在\n", 23, 0);
        return;
    }
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string query = "SELECT user_id, content, sent_at FROM messages WHERE room_id=" + std::to_string(roomId) +
        " ORDER BY sent_at DESC LIMIT 50";
    if (mysql_query(conn, query.c_str())) {
        mysql_close(conn);
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) { mysql_close(conn); return; }

    std::vector<std::string> historyLines;
    int rows = mysql_num_rows(res);
    for (int i = rows - 1; i >= 0; --i) {
        mysql_data_seek(res, i);
        MYSQL_ROW row = mysql_fetch_row(res);
        int userId = atoi(row[0]);
        std::string content = row[1];
        std::string sent_at = row[2];
        std::string nickname = getNicknameById(userId);
        historyLines.push_back("HISTORY|" + nickname + ": " + content + " (" + sent_at + ")");
    }
    mysql_free_result(res);
    mysql_close(conn);

    for (const auto& line : historyLines) {
        send(clientSocket, (line + "\n").c_str(), line.size() + 1, 0);
    }
    send(clientSocket, "HISTORY_END\n", 12, 0);
}

void processClientMessage(SOCKET clientSocket, const std::string& line) {
    size_t sep = line.find('|');
    if (sep == std::string::npos) return;
    std::string type = line.substr(0, sep);
    std::string data = line.substr(sep + 1);

    if (type == "MSG") handleMessage(clientSocket, data);
    else if (type == "CREATE") handleCreateRoom(clientSocket, data);
    else if (type == "JOIN") handleJoinRoom(clientSocket, data);
    else if (type == "LEAVE_ROOM") handleLeaveRoom(clientSocket);
    else if (type == "LIST_ROOMS") handleListRooms(clientSocket);
    else if (type == "PING") {
        std::lock_guard<std::mutex> lock(clients_mutex);
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
    }else if (type == "DISMISS_ROOM") {
        handleDismissRoom(clientSocket, data);
    }
}

void handle_client(SOCKET clientSocket) {
    std::string receiveBuffer;
    bool authenticated = false;
    std::string nickname;
    int userId = 0;

    // 认证阶段
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
                    send(clientSocket, okMsg.c_str(), okMsg.size(), 0);
                    Logger::instance().log("用户 " + username + " 登录成功，ID=" + std::to_string(userId));
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
                    Logger::instance().log("新用户注册: " + username);
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

    // 认证成功，加入全局列表和默认房间
    try {
        std::cout << "[INFO] 用户 " << nickname << " 认证成功，加入系统" << std::endl;

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(clientSocket);
            clientNicknames[clientSocket] = nickname;
            clientUserIds[clientSocket] = userId;
            clientLastHeartbeat[clientSocket] = std::chrono::steady_clock::now();
        }

        // 确保大厅存在
        {
            std::lock_guard<std::mutex> lockRooms(rooms_mutex);
            if (rooms.find(LOBBY_NAME) == rooms.end()) {
                Room hall;
                hall.name = LOBBY_NAME;
                hall.isPublic = true;
                hall.password = "";
                hall.ownerId = 0;
                rooms[LOBBY_NAME] = hall;
                createRoomInDB(LOBBY_NAME, true, "", 0);
                std::cout << "[INFO] 创建默认大厅房间" << std::endl;
            }
        }
        addToRoom(clientSocket, LOBBY_NAME);
        broadcastOnlineCount();  // 广播在线人数
        broadcastRoomList();     // 广播房间列表，让其他用户看到新成员
        handleListRooms(clientSocket); // 发送房间列表给当前用户

        // 主循环
        while (true) {
            char buffer[1024];
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (bytesReceived <= 0) {
                std::cout << "[INFO] 用户 " << nickname << " 断开连接" << std::endl;
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
        std::cerr << "[EXCEPTION] 处理客户端 " << nickname << " 时发生异常: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[EXCEPTION] 未知异常，客户端 " << nickname << " 断开" << std::endl;
    }

    // 清理
    cleanupClient(clientSocket);
    Logger::instance().log("用户 " + nickname + " 断开连接");
}

void startServer() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[ERROR] WSAStartup 失败" << std::endl;
        return;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "[ERROR] socket 创建失败" << std::endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(g_config.server.port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] bind 失败" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[ERROR] listen 失败" << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    std::cout << "[INFO] 服务器启动，监听端口 " << g_config.server.port << "..." << std::endl;

    loadRoomsFromDB();
    {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        if (rooms.find(LOBBY_NAME) == rooms.end()) {
            Room hall;
            hall.name = LOBBY_NAME;
            hall.isPublic = true;
            hall.password = "";
            hall.ownerId = 0;
            rooms[LOBBY_NAME] = hall;
            createRoomInDB(LOBBY_NAME, true, "", 0);
            std::cout << "[INFO] 创建默认大厅房间" << std::endl;
        }
    }

    // 启动心跳检测线程
    std::thread heartbeatChecker([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(g_config.heartbeat.check_interval_sec));
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::vector<SOCKET> toKill;
            for (auto it = clientLastHeartbeat.begin(); it != clientLastHeartbeat.end(); ++it) {
                SOCKET sock = it->first;
                auto last = it->second;
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
                if (elapsed > g_config.heartbeat.timeout_sec) {
                    std::cout << "[HEARTBEAT] 客户端 " << sock << " 心跳超时，断开" << std::endl;
                    toKill.push_back(sock);
                }
            }
            for (SOCKET sock : toKill) {
                cleanupClient(sock);
            }
        }
        });
    heartbeatChecker.detach();

    while (true) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "[ERROR] accept 失败" << std::endl;
            continue;
        }
        std::cout << "[INFO] 新客户端连接" << std::endl;
        std::thread(handle_client, clientSocket).detach();
    }

    closesocket(serverSocket);
    WSACleanup();
}