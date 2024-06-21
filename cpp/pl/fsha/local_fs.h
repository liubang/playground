//=====================================================================
//
// local_fs.h -
//
// Created by liubang on 2023/10/01 22:12
// Last Modified: 2023/10/01 22:12
//
//=====================================================================
#pragma once

#include "cpp/pl/fsha/fs_adaptor.h"

namespace pl {

class LocalFile : public FileAdaptor {
public:
    LocalFile(int fd, const std::string& name, ssize_t size)
        : FileAdaptor(name), fd_(fd), size_(size) {}
    virtual ~LocalFile() = default;

    ssize_t size() const override;
    ssize_t append(const void* buf, const size_t count) override;
    ssize_t read(void* buf, const size_t count, int64_t offset, bool* is_eof) override;
    int clear() override;
    int close() override;
    int sync() override;

private:
private:
    int fd_;
    ssize_t size_;
    bool direct_io_{false};
};

class LocalDir : public DirAdaptor {};

class LocalFs : public FsAdaptor {};

} // namespace pl
