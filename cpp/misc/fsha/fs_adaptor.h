//=====================================================================
//
// fs_adaptor.h -
//
// Created by liubang on 2023/09/28 14:02
// Last Modified: 2023/09/28 14:02
//
//=====================================================================
#pragma once

#include <ctime>
#include <memory>
#include <string>
#include <utility>

namespace pl {

constexpr static uint32_t FS_WRITE  = 1 << 0;
constexpr static uint32_t FS_TRUNC  = 1 << 1;
constexpr static uint32_t FS_DIRECT = 1 << 2;

enum class FsType {
    FST_LOCAL,
};

struct FileStatus {
    FsType type;
    ssize_t size;
    std::time_t last_mod_time;
};

class FileAdaptor {
public:
    FileAdaptor(std::string filename) : filename_(std::move(filename)) {}
    virtual ~FileAdaptor() = default;

    virtual ssize_t size() const = 0;
    virtual ssize_t append(const void* buf, const size_t count) = 0;
    virtual ssize_t read(void* buf, const size_t count, int64_t offset, bool* is_eof) = 0;
    virtual int clear() = 0;
    virtual int close() = 0;
    virtual int sync() { return 0; }
    std::string get_name() const { return filename_; }

protected:
    std::string filename_;
};

class DirAdaptor {
public:
    DirAdaptor(std::string dir) : dir_(std::move(dir)) {}
    virtual ~DirAdaptor() = default;

    virtual int next_entry(const std::string& filename, FsType* type) = 0;
    virtual int close() = 0;
    virtual void rewind() = 0;

protected:
    std::string dir_;
};

class FsAdaptor {
public:
    FsAdaptor() = default;
    virtual ~FsAdaptor() = default;

    virtual FsType type() const = 0;
    virtual std::unique_ptr<FileAdaptor> open(const std::string& filename, uint32_t flag) = 0;
    virtual int close(std::unique_ptr<FileAdaptor> file) = 0;
    virtual std::unique_ptr<FileAdaptor> create(const std::string& filename, uint32_t flag) = 0;
    virtual int unlink(const std::string& filename) = 0;
    virtual int stat(const std::string& filename, FileStatus* status) = 0;
    virtual ssize_t size(const std::string& filename) = 0;
    virtual bool is_file(const std::string& filename) = 0;
    virtual bool is_dir(const std::string& dir) = 0;
    virtual int rename(const std::string& oldname, const std::string& newname) = 0;
    virtual int mkdir(const std::string& filepath) = 0;
    virtual int rmdir(const std::string& filename, bool recursive = false) = 0;
    virtual std::unique_ptr<DirAdaptor> opendir(const std::string& dir) = 0;
    virtual int closedir(std::unique_ptr<DirAdaptor> dir) = 0;
};

} // namespace pl
