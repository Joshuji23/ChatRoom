#include "server.h"
#include "config.h"
#include "logger.h"

AppConfig g_config = loadConfig("config.json");

int main() {
    startServer();
    return 0;
}