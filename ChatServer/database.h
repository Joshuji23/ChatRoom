#ifndef DATABASE_H
#define DATABASE_H

#include <string>
#include <mysql.h>

bool authenticateUser(const std::string& username, const std::string& password, int& userId);
bool registerUser(const std::string& username, const std::string& password);
void createRoomInDB(const std::string& roomName, bool isPublic, const std::string& password, int ownerId);
void addMemberToRoomDB(int userId, const std::string& roomName);
void removeMemberFromRoomDB(int userId, const std::string& roomName);
void deleteRoomFromDB(const std::string& roomName);
void loadRoomsFromDB();

// 辅助函数
std::string getNicknameById(int userId);
int getRoomIdByName(const std::string& roomName);
MYSQL* connectDB();  // 声明，供其他模块使用
std::string escapeString(MYSQL* conn, const std::string& str);

#endif