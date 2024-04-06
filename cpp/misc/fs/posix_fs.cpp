//=====================================================================
//
// posix_fs.cpp -
//
// Created by liubang on 2023/05/28 01:44
// Last Modified: 2023/05/28 01:44
//
//=====================================================================
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <string>
#include <utility>

#include "cpp/misc/fs/fs.h"

namespace pl {

constexpr const size_t kWritableFileBufferSize = 65536;
constexpr const int kOpenBaseFlags = O_CLOEXEC;

Status posixError(const std::string& context, int err_number);

class PosixFsWriter final : public FsWriter {
public:
    PosixFsWriter(const PosixFsWriter&) = delete;

    PosixFsWriter(PosixFsWriter&&) = delete;

    PosixFsWriter& operator=(const PosixFsWriter&) = delete;

    PosixFsWriter& operator=(PosixFsWriter&&) = delete;

    PosixFsWriter(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {
        assert(fd >= 0);
    }

    ~PosixFsWriter() override {
        if (fd_ >= 0) {
            close();
        }
    }

    Status append(const Binary& data) override {
        std::size_t write_size = data.size();
        const char* write_data = data.data();
        std::size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
        std::memcpy(buf_ + pos_, write_data, copy_size);
        write_data += copy_size;
        write_size -= copy_size;
        pos_ += copy_size;
        if (write_size == 0) {
            return Status::NewOk();
        }

        auto status = flushBuffer();
        if (!status.isOk()) {
            return status;
        }

        if (write_size < kWritableFileBufferSize) {
            std::memcpy(buf_, write_data, write_size);
            pos_ = write_size;
            return Status::NewOk();
        }

        return writeUnbuffered(write_data, write_size);
    }

    Status close() override {
        Status status = flushBuffer();
        int result = ::close(fd_);
        if (result < 0 && status.isOk()) {
            status = posixError(filename_, errno);
        }
        fd_ = -1;
        return status;
    }

    Status flush() override { return flushBuffer(); }

    Status sync() override {
        Status status = flushBuffer();
        if (!status.isOk()) {
            return status;
        }
        return syncFd(fd_, filename_);
    }

private:
    Status writeUnbuffered(const char* data, std::size_t size) {
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
        return Status::NewOk();
    }

    Status flushBuffer() {
        Status status = writeUnbuffered(buf_, pos_);
        pos_ = 0;
        return status;
    }

    Status syncFd(int fd, const std::string& filename) {
        // 这里简单粗暴的区分Linux和macos
#if defined(__APPLE__) && defined(__MACH__)
        if (::fsync(fd) == 0) {
#else
        if (::fdatasync(fd) == 0) {
#endif
            return Status::NewOk();
        }
        return posixError(filename, errno);
    }

private:
    char buf_[kWritableFileBufferSize];
    std::size_t pos_{0};
    int fd_;
    const std::string filename_;
};

class PosixFsReader final : public FsReader {
public:
    PosixFsReader(const PosixFsReader&) = default;
    PosixFsReader(PosixFsReader&&) = default;
    PosixFsReader& operator=(const PosixFsReader&) = delete;
    PosixFsReader& operator=(PosixFsReader&&) = delete;

    PosixFsReader(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {
        assert(fd >= 0);
    }

    [[nodiscard]] std::size_t size() const override {
        if (fd_ >= 0) {
            struct stat file_stat;
            if (::fstat(fd_, &file_stat) == -1) {
                return 0;
            }
            return file_stat.st_size;
        }
        return 0;
    }

    ~PosixFsReader() override {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    };

    Status read(uint64_t offset, std::size_t n, Binary* result, char* scratch) const override {
        Status status;
        ssize_t read_size = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
        *result = Binary(scratch, (read_size < 0) ? 0 : read_size);
        if (read_size < 0) {
            status = posixError(filename_, errno);
        }
        return status;
    }

private:
    const std::string filename_;
    int fd_{0};
};

class PosixFs : public Fs {
public:
    PosixFs() = default;

    Status newFsWriter(const std::string& filename, FsWriter** result) override {
        int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
        if (fd < 0) {
            *result = nullptr;
            return posixError(filename, errno);
        }
        *result = new PosixFsWriter(filename, fd);
        return Status::NewOk();
    }

    Status newFsReader(const std::string& filename, FsReader** result) override {
        int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
        if (fd < 0) {
            *result = nullptr;
            return posixError(filename, errno);
        }
        *result = new PosixFsReader(filename, fd);
        return Status::NewOk();
    }
};

namespace {

template <typename T> class SingletonFs {
public:
    SingletonFs() { new (&fs_storage_) T(); }
    SingletonFs(const SingletonFs&) = delete;
    SingletonFs& operator=(const SingletonFs&) = delete;

    ~SingletonFs() = default;

    Fs* fs() { return reinterpret_cast<Fs*>(&fs_storage_); }

private:
    std::aligned_storage_t<sizeof(T), alignof(T)> fs_storage_;
};

using PosixDefaultFs = SingletonFs<PosixFs>;

} // namespace

Fs* Fs::getInstance() {
    static PosixDefaultFs posix_default_fs;
    return posix_default_fs.fs();
}

Status posixError(const std::string& context, int err_number) {
    if (errno == ENOENT) {
        return Status::NewNotFound(context + std::strerror(err_number));
    }
    return Status::NewIOError(context + std::strerror(err_number));
}

} // namespace pl
