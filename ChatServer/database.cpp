#include "database.h"
#include "utils.h"
#include "config.h"
#include <mysql.h>
#include <iostream>
#include <sstream>

extern AppConfig g_config;

MYSQL* connectDB() {
    MYSQL* conn = mysql_init(NULL);
    if (!conn) return nullptr;
    if (!mysql_real_connect(conn,
        g_config.db.host.c_str(),
        g_config.db.user.c_str(),
        g_config.db.password.c_str(),
        g_config.db.name.c_str(),
        g_config.db.port,
        NULL, 0)) {
        std::cerr << "[ERROR] MySQL 连接失败: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

std::string escapeString(MYSQL* conn, const std::string& str) {
    std::string result(2 * str.length() + 1, '\0');
    mysql_real_escape_string(conn, &result[0], str.c_str(), str.length());
    result.resize(strlen(result.c_str()));
    return result;
}

// 根据用户ID获取昵称
std::string getNicknameById(int userId) {
    MYSQL* conn = connectDB();
    if (!conn) return "未知";
    std::string query = "SELECT username FROM users WHERE id=" + std::to_string(userId);
    if (mysql_query(conn, query.c_str())) {
        mysql_close(conn);
        return "未知";
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        mysql_close(conn);
        return "未知";
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    std::string nickname = row[0];
    mysql_free_result(res);
    mysql_close(conn);
    return nickname;
}

// 根据房间名获取房间ID
int getRoomIdByName(const std::string& roomName) {
    MYSQL* conn = connectDB();
    if (!conn) return -1;
    std::string query = "SELECT id FROM rooms WHERE name='" + escapeString(conn, roomName) + "'";
    if (mysql_query(conn, query.c_str())) {
        mysql_close(conn);
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        mysql_close(conn);
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    int roomId = atoi(row[0]);
    mysql_free_result(res);
    mysql_close(conn);
    return roomId;
}

bool authenticateUser(const std::string& username, const std::string& password, int& userId) {
    std::cout << "[AUTH] 尝试认证: " << username << std::endl;
    MYSQL* conn = connectDB();
    if (!conn) return false;

    std::string escUser = escapeString(conn, username);
    std::string escPass = escapeString(conn, password);
    std::string query = "SELECT id FROM users WHERE username='" + escUser +
        "' AND password=SHA2('" + escPass + "', 256)";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[AUTH] 查询错误: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    bool ok = false;
    if (res && mysql_num_rows(res) > 0) {
        MYSQL_ROW row = mysql_fetch_row(res);
        userId = atoi(row[0]);
        ok = true;
        std::cout << "[AUTH] 成功，用户ID=" << userId << std::endl;
    }
    else {
        std::cout << "[AUTH] 失败" << std::endl;
    }
    if (res) mysql_free_result(res);
    mysql_close(conn);
    return ok;
}

bool registerUser(const std::string& username, const std::string& password) {
    std::cout << "[REG] 尝试注册: " << username << std::endl;
    MYSQL* conn = connectDB();
    if (!conn) return false;

    std::string escUser = escapeString(conn, username);
    std::string escPass = escapeString(conn, password);
    std::string query = "INSERT INTO users (username, password) VALUES ('" +
        escUser + "', SHA2('" + escPass + "', 256))";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[REG] 插入错误: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return false;
    }
    mysql_close(conn);
    std::cout << "[REG] 成功" << std::endl;
    return true;
}

void createRoomInDB(const std::string& roomName, bool isPublic, const std::string& password, int ownerId) {
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string escName = escapeString(conn, roomName);
    std::string escPass = password.empty() ? "NULL" : "'" + escapeString(conn, password) + "'";
    std::string query = "INSERT INTO rooms (name, is_public, password, owner_id) VALUES ('" +
        escName + "', " + (isPublic ? "1" : "0") + ", " +
        escPass + ", " + std::to_string(ownerId) + ")";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[DB] 创建房间失败: " << mysql_error(conn) << std::endl;
    }
    mysql_close(conn);
}

void addMemberToRoomDB(int userId, const std::string& roomName) {
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string escRoom = escapeString(conn, roomName);
    std::string query = "SELECT id FROM rooms WHERE name='" + escRoom + "'";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[DB] 查询房间ID失败: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res || mysql_num_rows(res) == 0) {
        if (res) mysql_free_result(res);
        mysql_close(conn);
        return;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    int roomId = atoi(row[0]);
    mysql_free_result(res);

    query = "INSERT IGNORE INTO room_memberships (user_id, room_id) VALUES (" +
        std::to_string(userId) + ", " + std::to_string(roomId) + ")";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[DB] 添加成员失败: " << mysql_error(conn) << std::endl;
    }
    mysql_close(conn);
}

void removeMemberFromRoomDB(int userId, const std::string& roomName) {
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string escRoom = escapeString(conn, roomName);
    std::string query = "DELETE rm FROM room_memberships rm JOIN rooms r ON rm.room_id=r.id WHERE r.name='" + escRoom + "' AND rm.user_id=" + std::to_string(userId);
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[DB] 移除成员失败: " << mysql_error(conn) << std::endl;
    }
    mysql_close(conn);
}

void deleteRoomFromDB(const std::string& roomName) {
    MYSQL* conn = connectDB();
    if (!conn) return;
    std::string escRoom = escapeString(conn, roomName);
    std::string query = "DELETE FROM rooms WHERE name='" + escRoom + "'";
    if (mysql_query(conn, query.c_str()) != 0) {
        std::cerr << "[DB] 删除房间失败: " << mysql_error(conn) << std::endl;
    }
    mysql_close(conn);
}

void loadRoomsFromDB() {
    std::cout << "[DB] 加载房间列表..." << std::endl;
    MYSQL* conn = connectDB();
    if (!conn) return;

    if (mysql_query(conn, "SELECT id, name, is_public, password, owner_id FROM rooms") != 0) {
        std::cerr << "[DB] 查询房间表失败: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        std::cerr << "[DB] mysql_store_result 失败" << std::endl;
        mysql_close(conn);
        return;
    }

    rooms.clear();
    int numRows = mysql_num_rows(res);
    for (int i = 0; i < numRows; ++i) {
        MYSQL_ROW row = mysql_fetch_row(res);
        int roomId = atoi(row[0]);
        std::string name = row[1];
        bool isPublic = (atoi(row[2]) != 0);
        std::string password = row[3] ? row[3] : "";
        int ownerId = row[4] ? atoi(row[4]) : 0;

        // 查询房间成员数
        std::string memberQuery = "SELECT COUNT(*) FROM room_memberships WHERE room_id=" + std::to_string(roomId);
        if (mysql_query(conn, memberQuery.c_str()) != 0) {
            std::cerr << "[DB] 查询成员数失败: " << mysql_error(conn) << std::endl;
            continue;
        }
        MYSQL_RES* memberRes = mysql_store_result(conn);
        if (!memberRes) {
            std::cerr << "[DB] 获取成员数结果失败" << std::endl;
            continue;
        }
        MYSQL_ROW memberRow = mysql_fetch_row(memberRes);
        int memberCount = memberRow ? atoi(memberRow[0]) : 0;
        mysql_free_result(memberRes);

        // 如果房间为空且不是大厅，则删除（清理遗留空房间）
        if (memberCount == 0 && name != "大厅") {
            std::cout << "[DB] 删除空房间: " << name << std::endl;
            deleteRoomFromDB(name);
            continue;
        }

        Room room;
        room.name = name;
        room.isPublic = isPublic;
        room.password = password;
        room.ownerId = ownerId;
        rooms[room.name] = room;
    }
    mysql_free_result(res);
    mysql_close(conn);
    std::cout << "[DB] 加载了 " << rooms.size() << " 个房间" << std::endl;
}