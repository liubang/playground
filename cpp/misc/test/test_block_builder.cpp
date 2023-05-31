#include "cpp/misc/sst/block_builder.h"
#include "cpp/misc/sst/encoding.h"
#include "cpp/tools/random.h"

#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
  // put your code here
  auto* comparator = playground::cpp::misc::sst::bytewiseComparator();
  std::unique_ptr<playground::cpp::misc::sst::Comparator> comparator_ptr(comparator);
  playground::cpp::misc::sst::BlockBuilder block_builder(comparator_ptr.get(), 16);
  constexpr int COUNT = 1000000;

  std::vector<std::string> keys;
  const std::string key_prefix = "test_key_";
  for (int i = 0; i < COUNT; ++i) {
    std::string key = key_prefix + std::to_string(i);
    keys.push_back(key);
  }

  std::sort(keys.begin(), keys.end());

  for (int i = 0; i < COUNT; ++i) {
    auto val = playground::cpp::tools::random_string(64);
    block_builder.add(keys[i], val);
  }

  auto block = block_builder.finish();
  // try to parse
  const char* data = block.data();
  std::size_t size = block.size();
  auto restart_count = playground::cpp::misc::sst::decodeInt<uint32_t>(&data[size - 4]);

  // parse all restarts
  std::vector<uint32_t> parsed_restarts;
  for (int i = 0; i < restart_count; ++i) {
    uint32_t offset = 4 * (i + 2);
    auto restart = playground::cpp::misc::sst::decodeInt<uint32_t>(&data[size - offset]);
    std::cout << restart << std::endl;
    parsed_restarts.push_back(restart);
  }

  return 0;
}
