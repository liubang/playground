#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

void signalHandler(int signum)
{
  std::cout << "quit:" << signum << std::endl;
  exit(signum);
}

int main(int argc, char *argv[])
{
  signal(SIGQUIT, signalHandler);
  for (;;) {
    std::cout << "Going to sleep...." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  return 0;
}
