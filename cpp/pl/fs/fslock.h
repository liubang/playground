#pragma once

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>

namespace pl {

class FsLock {
public:
    FsLock(std::string_view file) : _file(file) {}

    ~FsLock() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    // block
    bool lock() {
        if (!check_and_open_file()) {
            return false;
        }
        return 0 == ::flock(_fd, LOCK_EX);
    }

    // non-block
    bool try_lock() {
        if (!check_and_open_file()) {
            return false;
        }
        return 0 == ::flock(_fd, LOCK_EX | LOCK_NB);
    }

    bool unlock() {
        if (!check_and_open_file()) {
            return false;
        }
        return 0 == ::flock(_fd, LOCK_UN);
    }

private:
    bool check_and_open_file() {
        if (_fd < 0) {
            _fd = ::open(_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (_fd < 0) {
                std::cout << "failed to open file, error is " << std::strerror(errno) << '\n';
                return false;
            }
        }
        return true;
    }

private:
    std::string _file;
    int _fd{-1};
};

} // namespace pl
