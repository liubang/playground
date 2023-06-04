//=====================================================================
//
// status.h -
//
// Created by liubang on 2023/05/26 16:51
// Last Modified: 2023/05/26 16:51
//
//=====================================================================
#pragma once

#include <string>
#include <utility>

namespace playground::cpp::tools {

#define NEW_STATUS(name) \
  static Status New##name(const std::string& msg = "") { return {k##name, msg}; }

#define IS_STATUS(name) \
  [[nodiscard]] bool is##name() const { return code_ == k##name; }

class Status {
public:
  Status() = default;
  Status(const Status&) = default;

  Status& operator=(const Status&) = default;

  NEW_STATUS(Ok)
  NEW_STATUS(NotFound)
  NEW_STATUS(Corruption)
  NEW_STATUS(NotSupported)
  NEW_STATUS(InvalidArgument)
  NEW_STATUS(IOError)

  IS_STATUS(Ok)
  IS_STATUS(NotFound)
  IS_STATUS(Corruption)
  IS_STATUS(NotSupported)
  IS_STATUS(InvalidArgument)
  IS_STATUS(IOError)

  [[nodiscard]] const std::string& msg() const { return msg_; }

  [[nodiscard]] std::string to_string() const {
    // clang-format off
    switch (code_) {
    case kOk             : return "OK";
    case kNotFound       : return "NotFound";
    case kCorruption     : return "Corruption";
    case kNotSupported   : return "NotSupported";
    case kInvalidArgument: return "InvalidArgument";
    case kIOError        : return "IOError";
    default              : return "Unknown";
    }
    // clang-format on
  }

private:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5
  };

  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

private:
  Code code_{kOk};
  std::string msg_;
};

#undef IS_STATUS
#undef NEW_STATUS

}  // namespace playground::cpp::tools
