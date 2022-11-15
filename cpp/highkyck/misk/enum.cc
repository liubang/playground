#include <iostream>
#include <memory>

/*
struct UID {
  uint64_t value;
  constexpr explicit UID(uint64_t id = 0) noexcept : value(id) {}
  constexpr explicit operator bool() const noexcept {
    return value != 0;
  }
};

inline ::std::ostream& operator<<(::std::ostream& os, UID uid) {
  return os << uid.value;
}

class Player {
 public:
  UID uid() const;
};

class Card {
 public:
  uint64_t unique_id() const;
  uint64_t template_id() const;
};

::std::shared_ptr<Player> get_player_by_uid(UID uid) {
  if (!uid)
    return nullptr;
  std::cerr << uid << std::endl;
  return std::make_shared<Player>(10);
}
*/

enum UID : uint64_t;
enum CardID : uint64_t;

int main(int argc, char* argv[]) {
  return 0;
}
