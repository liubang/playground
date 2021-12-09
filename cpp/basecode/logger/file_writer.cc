#include "basecode/logger/file_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace basecode {
namespace logger {

namespace {
class MmapFileWriter : public FileWriter {
 public:
  MmapFileWriter(const std::string& basename, uint32_t mem_size)
      : mem_size_(mem_size) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = open(basename.data(), O_RDWR | O_CREAT,
               S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd_ < 0) {
      fprintf(stderr, "open new file failed, errno=%d", errno);
    } else {
      (void)ftruncate(fd_, mem_size);
      buffer_ = (char*)mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            fd_, 0);
      if (buffer_ == MAP_FAILED) {
        fprintf(stderr, "mmap file failed, errno=%d", errno);
      }
    }
  }

  ~MmapFileWriter() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (buffer_ != MAP_FAILED) {
      munmap(buffer_, mem_size_);
    }
  }

  void append(const char* msg, int32_t len) {
    if (len > mem_size_ - writed_) {
      fprintf(stderr, "mmap memory overflow, errno=%d", errno);
      return;
    }
    memcpy(buffer_ + writed_, msg, len);
    writed_ += len;
  }

  void flush() {
    if (buffer_ != MAP_FAILED) {
      msync(buffer_, mem_size_, MS_ASYNC);
    }
  }

  uint32_t write_bytes() const { return writed_; }

 private:
  int fd_;
  char* buffer_;
  int32_t mem_size_;
  int32_t writed_{0};
};

class AppendFileWriter : public FileWriter {
 public:
  AppendFileWriter(const std::string& filename)
      : fp_(::fopen(filename.data(), "ae")) {
    ::setbuffer(fp_, buffer_, sizeof buffer_);
  }

  ~AppendFileWriter() {
    if (fp_) {
      ::fclose(fp_);
    }
  }

  void append(const char* msg, int32_t len) {
    size_t n = fwrite_unlocked(msg, 1, len, fp_);
    size_t remain = len - n;
    while (remain > 0) {
      size_t x = fwrite_unlocked(msg + n, 1, remain, fp_);
      if (x == 0) {
        int err = ferror(fp_);
        if (err) {
          fprintf(stderr, "AppendFileWriter::append() failed %s\n",
                  strerror(err));
        }
        break;
      }
      n += x;
      remain = len - n;
    }
    writed_ += len;
  }

  void flush() { fflush(fp_); }

  uint32_t write_bytes() const { return writed_; }

 private:
  FILE* fp_;
  char buffer_[64 * 1024];
  uint32_t writed_{0};
};

std::string get_hostname() {
  char buf[256];
  if (gethostname(buf, sizeof buf) == 0) {
    buf[sizeof(buf) - 1] = '\0';
    return buf;
  }
  return "unknownhost";
}

std::string get_log_filename(const std::string& basename, time_t* now) {
  std::string filename;
  filename.reserve(basename.size() + 64);
  filename = basename;

  char timebuf[32];
  struct tm tm;

  *now = time(NULL);
  gmtime_r(now, &tm);
  strftime(timebuf, sizeof timebuf, ".%Y%m%d-%H%M%S.", &tm);
  filename += timebuf;
  filename += get_hostname();

  char pidbuf[32];
  snprintf(pidbuf, sizeof pidbuf, ".%d", getpid());
  filename += pidbuf;

  filename += ".log";
  return filename;
}

}  // namespace

LogFile::LogFile(const std::string& basename, int32_t roll_size,
                 int32_t flush_interval, int32_t check_interval,
                 FileWriterType file_writer_type)
    : basename_(basename),
      roll_size_(roll_size),
      flush_interval_(flush_interval),
      check_freq_count_(check_interval),
      count_(0),
      start_of_period_(0),
      last_roll_(0),
      last_flush_(0) {
  time_t now = 0;
  std::string filename = get_log_filename(basename_, &now);
  if (file_writer_type == FileWriterType::MMAP_FILE) {
    file_ = std::make_shared<MmapFileWriter>(filename, roll_size_);
  } else {
    file_ = std::make_shared<AppendFileWriter>(filename);
  }
  file_writer_type_ = file_writer_type;
  roll_file();
}

void LogFile::append(const char* msg, int32_t len) {
  file_->append(msg, len);
  if (file_->write_bytes() > roll_size_) {
    roll_file();
  } else {
    ++count_;
    if (count_ >= check_freq_count_) {
      count_ = 0;
      time_t now = time(NULL);
      time_t this_period = now / kRollPerSeconds * kRollPerSeconds;
      if (this_period != start_of_period_) {
        roll_file();
      } else if (now - last_flush_ > flush_interval_) {
        last_flush_ = now;
        file_->flush();
      }
    }
  }
}

void LogFile::flush() { file_->flush(); }

bool LogFile::roll_file() {
  time_t now = 0;
  std::string filename = get_log_filename(basename_, &now);
  time_t start = now / kRollPerSeconds * kRollPerSeconds;

  if (now > last_roll_) {
    last_roll_ = now;
    last_flush_ = now;
    start_of_period_ = start;
    if (file_writer_type_ == FileWriterType::MMAP_FILE) {
      file_.reset(new MmapFileWriter(filename, roll_size_));
    } else {
      file_.reset(new AppendFileWriter(filename));
    }
  }

  return true;
}

}  // namespace logger
}  // namespace basecode
