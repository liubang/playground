//=====================================================================
//
// posix_fs.cpp -
//
// Created by liubang on 2023/05/28 01:44
// Last Modified: 2023/05/28 01:44
//
//=====================================================================
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <utility>

#include "cpp/misc/fs/fs.h"

namespace playground::cpp::misc::fs {

constexpr const size_t kWritableFileBufferSize = 65536;
constexpr const int kOpenBaseFlags = O_CLOEXEC;

tools::Status posixError(const std::string& context, int err_number) {
  if (errno == ENOENT) {
    return tools::Status::NewNotFound(context + std::strerror(err_number));
  }
  return tools::Status::NewIOError(context + std::strerror(err_number));
}

class PosixFsWriter final : public FsWriter {
public:
  PosixFsWriter(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {}

  ~PosixFsWriter() override {
    if (fd_ >= 0) {
      close();
    }
  }

  tools::Status append(const tools::Binary& data) override {
    std::size_t write_size = data.size();
    const char* write_data = data.data();
    std::size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_data += copy_size;
    write_size -= copy_size;
    pos_ += copy_size;
    if (write_size == 0) {
      return tools::Status::NewOk();
    }

    auto status = flushBuffer();
    if (!status.isOk()) {
      return status;
    }

    if (write_size < kWritableFileBufferSize) {
      std::memcpy(buf_, write_data, write_size);
      pos_ = write_size;
      return tools::Status::NewOk();
    }

    return writeUnbuffered(write_data, write_size);
  }

  tools::Status close() override {
    tools::Status status = flushBuffer();
    int result = ::close(fd_);
    if (result < 0 && status.isOk()) {
      status = posixError(filename_, errno);
    }
    fd_ = -1;
    return status;
  }

  tools::Status flush() override { return flushBuffer(); }

  tools::Status sync() override {
    tools::Status status = flushBuffer();
    if (!status.isOk()) {
      return status;
    }
    return syncFd(fd_, filename_);
  }

private:
  tools::Status writeUnbuffered(const char* data, std::size_t size) {
    while (size > 0) {
      ssize_t write_result = ::write(fd_, data, size);
      if (write_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        return posixError(filename_, errno);
      }
      data += write_result;
      size -= write_result;
    }
    return tools::Status::NewOk();
  }

  tools::Status flushBuffer() {
    tools::Status status = writeUnbuffered(buf_, pos_);
    pos_ = 0;
    return status;
  }

  tools::Status syncFd(int fd, const std::string& filename) {
    // 这里简单粗暴的区分Linux和macos
#if defined(__APPLE__) && defined(__MACH__)
    if (::fsync(fd) == 0) {
#else
    if (::fdatasync(fd) == 0) {
#endif
      return tools::Status::NewOk();
    }
    return posixError(filename, errno);
  }

private:
  char buf_[kWritableFileBufferSize];
  std::size_t pos_{0};
  int fd_;
  const std::string filename_;
};

class PosixFs : public Fs {
public:
  PosixFs() = default;

  tools::Status newFsWriter(const std::string& filename, FsWriter** result) override {
    int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
      *result = nullptr;
      return posixError(filename, errno);
    }
    *result = new PosixFsWriter(filename, fd);
    return tools::Status::NewOk();
  }

  tools::Status newFsReader(const std::string& filename, FsReader** result) override {
    // TODO(liubang): implement
    return tools::Status::NewOk();
  }
};

namespace {

template <typename T>
class SingletonFs {
public:
  SingletonFs() { new (&fs_storage_) T(); }
  SingletonFs(const SingletonFs&) = delete;
  SingletonFs& operator=(const SingletonFs&) = delete;

  ~SingletonFs() = default;

  Fs* fs() { return reinterpret_cast<Fs*>(&fs_storage_); }

private:
  typename std::aligned_storage<sizeof(T), alignof(T)>::type fs_storage_;
};

using PosixDefaultFs = SingletonFs<PosixFs>;

}  // namespace

Fs* Fs::getInstance() {
  static PosixDefaultFs posix_default_fs;
  return posix_default_fs.fs();
}

}  // namespace playground::cpp::misc::fs
