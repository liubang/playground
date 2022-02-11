#include "value_type_of.h"

#include <vector>

int main(int argc, char* argv[]) {
  static_assert(std::is_same_v<highkyck::meta::value_type_of<std::vector<int>>, int>);
  static_assert(std::is_same_v<highkyck::meta::value_type_of<float[3]>, float>);
  return 0;
}
