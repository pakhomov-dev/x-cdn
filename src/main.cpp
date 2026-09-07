#include "xcdn_server.h"
#include <signal.h>
#include <thread>
#include <chrono>

extern "C" void handle_signal(int);
int main() {
    // Настраиваем обработку Ctrl+C
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    XServer server(8080, "public");   // порт 8080, файлы из текущей папки
    server.start();

    // Ждём завершения (основной поток ничего не делает)
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}