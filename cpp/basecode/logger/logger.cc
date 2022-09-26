#include "basecode/logger/logger.h"

#include <stdarg.h>
#include <time.h>

#include <cstring>
#include <vector>

namespace basecode {
namespace logger {

std::string get_log_level_str(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG:
      return "DEBUG";
      break;
    case LogLevel::INFO:
      return "INFO";
      break;
    case LogLevel::WARN:
      return "WARN";
      break;
    case LogLevel::ERROR:
      return "ERROR";
      break;
    case LogLevel::FATAL:
      return "FATAL";
      break;
    default:
      return "UNKNOWN";
  }
}

std::string now_time_str() {
  time_t now;
  time(&now);
  char res[64];
  strftime(res, sizeof(res), "%Y-%m-%d %H:%M:%S", localtime(&now));
  return res;
}

void Logger::debug(const char* format, ...) {
  if (!format) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  write_log(LogLevel::DEBUG, __FILE__, __FUNCTION__, __LINE__, format, ap);
  va_end(ap);
}

void Logger::info(const char* format, ...) {
  if (!format) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  write_log(LogLevel::INFO, __FILE__, __FUNCTION__, __LINE__, format, ap);
  va_end(ap);
}

void Logger::warn(const char* format, ...) {
  if (!format) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  write_log(LogLevel::WARN, __FILE__, __FUNCTION__, __LINE__, format, ap);
  va_end(ap);
}

void Logger::error(const char* format, ...) {
  if (!format) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  write_log(LogLevel::ERROR, __FILE__, __FUNCTION__, __LINE__, format, ap);
  va_end(ap);
}

void Logger::fatal(const char* format, ...) {
  if (!format) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  write_log(LogLevel::FATAL, __FILE__, __FUNCTION__, __LINE__, format, ap);
  va_end(ap);
}

void Logger::write_log(LogLevel level,
                       const char* file_name,
                       const char* function_name,
                       int32_t line_num,
                       const char* fmt,
                       va_list ap) {
  if (level < kLoggerConfig.level) {
    return;
  }
  std::string str_result;
  if (fmt != nullptr) {
    size_t len = vprintf(fmt, ap) + 1;
    std::vector<char> fmt_bufs(len, '\0');
    int write_n = vsnprintf(&fmt_bufs[0], fmt_bufs.size(), fmt, ap);
    if (write_n > 0) {
      str_result = &fmt_bufs[0];
    }
  }
  if (str_result.empty()) {
    return;
  }
  const auto& get_source_file_name = [](const char* file_name) {
    return strrchr(file_name, '/') ? strrchr(file_name, '/') + 1 : file_name;
  };

  std::string prefix;
  prefix.append(now_time_str() + "-");
  prefix.append(get_log_level_str(level) + "-");
  prefix.append(get_source_file_name(file_name));
  prefix.append("-");
  prefix.append(function_name);
  prefix.append("-");
  prefix.append(std::to_string(line_num) + "-");
  prefix.append(str_result);
  while (!functors_.empty()) {
    Task task = std::move(functors_.front());
    functors_.pop_front();
    task();
  }
  std::unique_lock<std::mutex> lk(mutex_);
  for (const auto& appender : appenders_) {
    appender.second->append(prefix.data(), prefix.size());
  }
}

void Logger::add_appender(const std::string& name,
                          LogAppenderInterface::Ptr appender) {
  std::unique_lock<std::mutex> lk(mutex_);
  appenders_[name] = appender;
}

void Logger::del_appender(const std::string& name) {
  std::unique_lock<std::mutex> lk(mutex_);
  for (auto it = appenders_.begin(); it != appenders_.end();) {
    if (it->first == name) {
      it = appenders_.erase(it);
    } else {
      ++it;
    }
  }
}

void Logger::clear_appender() {
  appenders_.clear();
}

}  // namespace logger
}  // namespace basecode
