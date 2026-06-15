#ifndef DATABASE_H
#define DATABASE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <mysql.h>

bool authenticateUser(const std::string& username, const std::string& password, int& userId);
bool registerUser(const std::string& username, const std::string& password);
void createRoomInDB(const std::string& roomName, bool isPublic, const std::string& password, int ownerId);
void addMemberToRoomDB(int userId, const std::string& roomName);
void removeMemberFromRoomDB(int userId, const std::string& roomName);
void deleteRoomFromDB(const std::string& roomName);
void loadRoomsFromDB();

std::string getNicknameById(int userId);
int getRoomIdByName(const std::string& roomName);
MYSQL* connectDB();
std::string escapeString(MYSQL* conn, const std::string& str);

#endif