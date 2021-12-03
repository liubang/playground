#pragma once

#include <functional>
#include <sys/types.h>

namespace basecode {
namespace logger {

class Buffer
{
public:
    using CookieFunc = std::function<void()>;

public:
    Buffer(size_t total = 1024 * 1024 * 10);

    ~Buffer();

    void clear();

    void append(const char* data, size_t len);

    const char* data() const;

    size_t length() const;

    size_t available() const;

    const char* debug();

    void set_cookie(const CookieFunc& cookie);

private:
    static void cookie_start();

    static void cookie_end();

private:
    const size_t total_;
    size_t       available_;
    size_t       cur_;
    char*        data_;
    CookieFunc   cookie_;
};

}  // namespace logger
}  // namespace basecode
