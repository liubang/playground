#pragma once

#include <vector>
#include <string>

#include "result_message.h"

namespace basecode {
class Result
{
public:
  Result() = default;

  inline void fail() { success_ = false; }

  inline void succed() { success_ = true; }

  inline void info(const std::string& code, const std::string& message,
                   const std::string& details = "")
  {
    messages_.emplace_back(code, message, details, ResultMessage::Types::INFO);
  }

  inline void warning(const std::string& code, const std::string& message,
                      const std::string& details = "")
  {
    messages_.emplace_back(code, message, details, ResultMessage::Types::WARNING);
  }

  inline void error(const std::string& code, const std::string& message,
                    const std::string& details = "")
  {
    messages_.emplace_back(code, message, details, ResultMessage::Types::ERROR);
  }

  inline bool is_failed() const { return !success_; }

  void remove_code(const std::string& code)
  {
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
      if ((*it).code() == code) { it = messages_.erase(it); }
    }
  }

  inline const ResultMessageList& messages() const { return messages_; }

  inline bool has_code(const std::string& code) const
  {
    for (const auto& message : messages_) {
      if (message.code() == code) { return true; }
    }
    return false;
  }

  inline const ResultMessage* find_code(const std::string& code) const
  {
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
      if ((*it).code() == code) { return &(*it); }
    }
    return nullptr;
  }

private:
  bool success_{true};
  ResultMessageList messages_{};
};
}  // namespace basecode
