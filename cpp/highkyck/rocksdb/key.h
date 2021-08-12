#pragma once

#include <string>
#include <msgpack.hpp>

namespace highkyck {
class Key
{
public:
  Key() {}
  std::string dbname_;
  std::string tbname_;
  std::string key_;

  MSGPACK_DEFINE(dbname_, tbname_, key_);
};
}   // namespace highkyck
