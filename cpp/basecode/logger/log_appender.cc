#include "basecode/logger/log_appender.h"
#include "basecode/logger/logger.h"
#include <sys/stat.h>

namespace basecode {
namespace logger {

AsyncLogAppender::AsyncLogAppender(const std::string& basename)
  : started_(false)
  , running_(false)
  , persist_period_(kLoggerConfig.file_option.log_flush_interval)
  , basename_(basename)
  , countdown_latch_(1)
  , cur_buffer_(new Buffer(kLoggerConfig.log_buffer_size))
{
  mkdir(basename_.data(), 0755);
  start();
}

AsyncLogAppender::~AsyncLogAppender()
{
  if (started_) { stop(); }
}

void AsyncLogAppender::append(const char* msg, size_t len)
{
  std::unique_lock<std::mutex> lk(mutex_);
  if (cur_buffer_->available() >= len) {
    cur_buffer_->append(msg, len);
  } else {
    buffers_.push_back(std::move(cur_buffer_));
    cur_buffer_.reset(new Buffer(kLoggerConfig.log_buffer_size));
    cur_buffer_->append(msg, len);
    cound_.notify_one();
  }
}

void AsyncLogAppender::start()
{
  started_ = true;
  running_ = true;
  persist_thread_ = std::thread([this]() { thread_func(); });
  countdown_latch_.await();
}

void AsyncLogAppender::stop()
{
  started_ = false;
  cound_.notify_all();
  persist_thread_.join();
}

void AsyncLogAppender::thread_func()
{
  std::unique_ptr<Buffer> buffer(new Buffer(kLoggerConfig.log_buffer_size));
  std::vector<std::unique_ptr<Buffer>> persist_buffers;
  persist_buffers.reserve(kLoggerConfig.log_buffer_nums);
  LogFile log_file(basename_, kLoggerConfig.file_option.log_flush_file_size,
                   kLoggerConfig.file_option.log_flush_interval, 1024,
                   kLoggerConfig.file_option.file_writer_type);
  countdown_latch_.count_down();
  while (running_) {
    {
      std::unique_lock<std::mutex> lk(mutex_);
      if (buffers_.empty()) { cound_.wait_for(lk, std::chrono::seconds(1)); }
      if (buffers_.empty() && cur_buffer_->length() == 0) { continue; }

      buffers_.push_back(std::move(cur_buffer_));
      persist_buffers.swap(buffers_);
      cur_buffer_ = std::move(buffer);
      cur_buffer_->clear();
    }

    if (persist_buffers.size() > kLoggerConfig.log_buffer_size) {
      persist_buffers.erase(persist_buffers.begin() + 1, persist_buffers.end());
    }

    for (const auto& buffer : persist_buffers) {
      log_file.append(buffer->data(), buffer->length());
    }

    buffer = std::move(persist_buffers[0]);
    buffer->clear();
    persist_buffers.clear();
    log_file.flush();
    if (!started_) {
      std::unique_lock<std::mutex> lk(mutex_);
      if (cur_buffer_->length() == 0) { running_ = false; }
    }
  }
  log_file.flush();
}

}  // namespace logger
}  // namespace basecode
