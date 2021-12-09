#include <gtest/gtest.h>

#include <list>
#include <unordered_map>

namespace {
class LRUCache {
 public:
  LRUCache(int capacity) : capacity_(capacity) {}

  void put(int key, int val) {
    if (map_.find(key) == map_.end()) {
      if (cache_.size() == capacity_) {
        map_.erase(cache_.back().first);
        cache_.pop_back();
      }
    } else {
      cache_.erase(map_[key]);
    }
    cache_.push_front({key, val});
    map_[key] = cache_.begin();
  }

  int get(int key) {
    if (map_.find(key) == map_.end()) {
      return -1;
    }
    std::pair<int, int> kv = *map_[key];
    cache_.erase(map_[key]);
    cache_.push_front(kv);
    map_[key] = cache_.begin();
    return kv.second;
  }

 private:
  int capacity_;
  std::list<std::pair<int, int>> cache_;
  std::unordered_map<int, std::list<std::pair<int, int>>::iterator> map_;
};
}  // namespace

TEST(Leetcode, lru_cache) {
  LRUCache cache(2);
  cache.put(1, 1);
  cache.put(2, 2);
  EXPECT_EQ(1, cache.get(1));
  cache.put(3, 3);
  EXPECT_EQ(-1, cache.get(2));
  cache.put(4, 4);
  EXPECT_EQ(-1, cache.get(1));
  EXPECT_EQ(3, cache.get(3));
  EXPECT_EQ(4, cache.get(4));
}
