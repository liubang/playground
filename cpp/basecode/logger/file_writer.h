#pragma once

#include <stdint.h>

#include <memory>
#include <string>

namespace basecode {
namespace logger {

enum class FileWriterType : uint8_t {
  MMAP_FILE = 0,
  APPEND_FILE = 1,
};

class FileWriter {
 public:
  FileWriter() = default;

  virtual ~FileWriter() = default;

  virtual void append(const char* msg, int32_t len) = 0;

  virtual void flush() = 0;

  virtual uint32_t write_bytes() const = 0;
};

class LogFile {
 public:
  LogFile(const std::string& basename,
          int32_t roll_size,
          int32_t flush_interval,
          int32_t check_interval,
          FileWriterType file_writer_type);

  ~LogFile() = default;

  void append(const char* msg, int32_t len);

  void flush();

  bool roll_file();

 private:
  std::string basename_;
  uint32_t roll_size_;
  uint32_t flush_interval_;
  uint32_t check_freq_count_;
  uint32_t count_;
  time_t start_of_period_;
  time_t last_roll_;
  time_t last_flush_;
  std::shared_ptr<FileWriter> file_;
  FileWriterType file_writer_type_;

 private:
  constexpr static int kRollPerSeconds = 60 * 60 * 24;
};

}  // namespace logger
}  // namespace basecode
