//=====================================================================
//
// strconv.h -
//
// Created by liubang on 2023/11/04 17:26
// Last Modified: 2023/11/04 17:26
//
//=====================================================================

#include <optional>
#include <string>

namespace pl {

template <typename T, typename E> class Result {
public:
    static Result<T, E> Err(E&& ee) {
        Result<T, E> ret;
        ret.e_ = std::move(ee);
        ret.ok_ = false;
        return std::move(ret);
    }

    static Result<T, E> Ok(T&& tt) {
        Result<T, E> ret;
        ret.t_ = std::move(tt);
        ret.ok_ = true;
        return std::move(ret);
    }

    bool ok() const { return ok_; }
    bool err() const { return !ok_; }

    const T& t() const { return t_; }
    const E& e() const { return e_; }

private:
    Result() = default;

private:
    T t_;
    E e_;
    bool ok_;
};

class StrConv {
public:
    static bool to_byte(char c, uint8_t* b) {
        if (c >= '0' && c <= '9') {
            *b = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            *b = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            *b = c - 'A' + 10;
        } else {
            return false;
        }
        return true;
    }

    static Result<std::string, std::string>
    push_hex_byte(const std::string& lit, size_t start, std::string* s) {
        if (start == lit.length() - 1) {
            return Result<std::string, std::string>::Err(R"(\x followed by 0 char, must be 2)");
        }
        auto ch1 = lit[++start];
        if (start == lit.length() - 1) {
            return Result<std::string, std::string>::Err(R"(\x followed by 1 char, must be 2)");
        }
        auto ch2 = lit[++start];
        uint8_t b1, b2;
        if (!to_byte(ch1, &b1) || !to_byte(ch2, &b2)) {
            return Result<std::string, std::string>::Err("invalid byte value");
        }
        uint8_t b = (b1 << 4) | b2;
        s->push_back(static_cast<unsigned char>(b));
        return Result<std::string, std::string>::Ok("");
    }

    static Result<std::string, std::string> parse_text(const std::string& lit) {
        std::string s;
        for (size_t i = 0; i < lit.length();) {
            char c = lit[i];
            if (c == '\\') {
                ++i;
                if (i == lit.length()) {
                    return Result<std::string, std::string>::Err("invalid escape sequence");
                }
                char n = lit[i];
                switch (n) {
                case 'n':
                    s.push_back('\n');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case '\\':
                    s.push_back('\\');
                    break;
                case '"':
                    s.push_back('"');
                    break;
                case '$':
                    s.push_back('$');
                    break;
                case 'x': {
                    auto ret = push_hex_byte(lit, i, &s);
                    if (!ret.ok()) {
                        return ret;
                    }
                    break;
                }
                default:
                    return Result<std::string, std::string>::Err("invalid escape character " +
                                                                 std::string(n, 1));
                }
            }
        }
        return Result<std::string, std::string>::Ok(std::move(s));
    }

    static Result<std::string, std::string> parse_string(const std::string& lit) {
        if (lit.length() < 2 || !lit.starts_with('"') || !lit.ends_with('"')) {
            return Result<std::string, std::string>::Err("invalid string literal");
        }
        return parse_text(lit.substr(1, lit.length() - 2));
    }
};

} // namespace pl
