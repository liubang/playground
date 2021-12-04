#pragma once

#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>

#include "basecode/logger/file_writer.h"
#include "basecode/logger/log_appender.h"
#include "basecode/singleton.h"

namespace basecode {
namespace logger {

enum class LogLevel : uint8_t {
  DEBUG = 0,
  INFO = 1,
  WARN = 2,
  ERROR = 3,
  FATAL = 4,
};

struct LoggerConfig {
  uint32_t log_buffer_size = 4000;
  uint32_t log_buffer_nums = 2;
  LogLevel level = LogLevel::INFO;

  struct FileOption {
    std::string file_path;
    uint32_t log_flush_file_size;
    uint32_t log_flush_interval;
    FileWriterType file_writer_type;
  } file_option;
};

static LoggerConfig kLoggerConfig;

class Logger {
public:
  static Logger *get_logger() {
    return basecode::Singleton<Logger>::get_instance();
  }

  static void set_global_config(const LoggerConfig &conf) {
    kLoggerConfig = conf;
  }

  template <class F, class... Args>
  void register_handler(F &&f, Args &&...args) {
    using RetType = decltype(f(args...));
    auto task = std::make_shared<RetType>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    functors_.emplace([task]() { (*task)(); });
  }

  void info(const char *format, ...);

  void debug(const char *format, ...);

  void warn(const char *format, ...);

  void error(const char *format, ...);

  void fatal(const char *format, ...);

  void add_appender(const std::string &name,
                    LogAppenderInterface::Ptr appender);

  void del_appender(const std::string &name);

  void clear_appender();

private:
  void write_log(LogLevel level, const char *file_name,
                 const char *function_name, int32_t line_num, const char *fmt,
                 va_list ap);

private:
  using Task = std::function<void()>;
  std::mutex mutex_;
  std::unordered_map<std::string, LogAppenderInterface::Ptr> appenders_;
  std::list<Task> functors_;
};

} // namespace logger
} // namespace basecode
