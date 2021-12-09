#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace highkyck {
namespace common {

template <typename T>
void print(T t) {
  std::cout << t;
}

template <typename Kt, typename Vt>
void print(std::pair<Kt, Vt> kv) {
  print(kv.first);
  print(" = ");
  print(kv.second);
}

template <>
void print(const std::string& s) {
  std::cout << s;
}

template <typename T, typename AllocT,
          template <typename, typename...> typename SequenceT>
void print(SequenceT<T, AllocT> seq) {
  print("{ ");
  for (auto iter = std::begin(seq); iter != std::end(seq);
       iter = std::next(iter)) {
    print(*iter);
    print(",");
  }
  print("\b}");
}

}  // namespace common
}  // namespace highkyck
