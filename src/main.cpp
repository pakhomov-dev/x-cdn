#include "xcdn_config.h"
#include "xcdn_server.h"
#include <chrono>
#include <signal.h>
#include <thread>

extern "C" void handle_signal(int);
int main(int argc, char *argv[]) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  ServerConfig cfg = parse_args(argc, argv);
  XServer server(cfg);
  server.start();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return 0;
}
