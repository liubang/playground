#pragma once

#include <unistd.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "basecode/count_down_latch.h"
#include "basecode/logger/buffer.h"

namespace basecode {
namespace logger {

class LogAppenderInterface {
 public:
  virtual ~LogAppenderInterface() {}

  virtual void append(const char* msg, size_t len) = 0;

 public:
  using Ptr = std::shared_ptr<LogAppenderInterface>;
};

class AsyncLogAppender : public LogAppenderInterface {
 public:
  AsyncLogAppender(const std::string& basename);

  ~AsyncLogAppender();

  void append(const char* msg, size_t len);

  void start();

  void stop();

 private:
  void thread_func();

 private:
  bool started_;
  bool running_;
  time_t persist_period_;
  std::string basename_;
  std::mutex mutex_;
  std::condition_variable cound_;
  basecode::CountDownLatch countdown_latch_;
  std::thread persist_thread_;
  std::unique_ptr<Buffer> cur_buffer_;
  std::vector<std::unique_ptr<Buffer>> buffers_;
};

}  // namespace logger
}  // namespace basecode
