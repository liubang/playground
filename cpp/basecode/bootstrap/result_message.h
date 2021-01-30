#pragma once

#include <vector>
#include <string>

namespace basecode {
class ResultMessage {
 public:
  enum Types {
    INFO,
    ERROR,
    WARNING,
    DATA,
  };

  ResultMessage(
      const std::string& code,
      const std::string& message,
      const std::string& details = "",
      Types type = Types::INFO)
      : type_(type), code_(code), message_(message), details_(details) {}

  inline Types type() const {
    return type_;
  }

  inline bool is_error() const {
    return type_ == Types::ERROR;
  }

  inline const std::string& code() const {
    return code_;
  }

  inline const std::string& details() const {
    return details_;
  }

  inline const std::string& message() const {
    return message_;
  }

 private:
  Types type_;
  std::string code_;
  std::string message_;
  std::string details_;
};

using ResultMessageList = std::vector<ResultMessage>;

} // namespace basecode
