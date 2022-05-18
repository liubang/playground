#pragma once

#include <list>
#include <optional>
#include <unordered_map>

namespace highkyck::cache {

template<typename K, typename V, std::size_t Capacity> class LRUCache
{
public:
  static_assert(Capacity > 0);

  bool put(const K &k, const V &v);

  std::optional<V> get(const K &k);

  void erase(const K &k);

  template<typename C> void forEach(const C &cb) const
  {
    for (auto &[k, v] : items_) { cb(k, v); }
  }

private:
  std::list<std::pair<K, V>> items_;
  std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> index_;
};


template<typename K, typename V, std::size_t Capacity>
bool LRUCache<K, V, Capacity>::put(const K &k, const V &v)
{
  if (index_.count(k) > 0) { return false; }

  if (items_.size() == Capacity) {
    index_.erase(items_.back().first);
    items_.pop_back();
  }

  items_.emplace_front(k, v);
  index_.emplace(k, items_.begin());

  return true;
}

}// namespace highkyck::cache
