#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>

namespace {
class Trie {
 public:
  Trie() { head_ = new Node(); }

  ~Trie() {
    std::function<void(Node*)> destroy = [&](Node* node) {
      for (Node* n : node->nodes) {
        destroy(n);
      }
      delete node;
    };
  }

  void insert(const std::string& word) {
    auto cur = head_;
    for (auto& c : word) {
      int idx = c - 'a';
      if (!cur->nodes[idx]) {
        cur->nodes[idx] = new Node(c);
      }
      cur = cur->nodes[idx];
    }
    cur->is_word = true;
  }

  bool search(const std::string& word) {
    auto cur = head_;
    for (auto& c : word) {
      int idx = c - 'a';
      if (!cur->nodes[idx]) {
        return false;
      }
      cur = cur->nodes[idx];
    }
    return cur->is_word;
  }

  bool prefix(const std::string& prefix) {
    auto cur = head_;
    for (auto& c : prefix) {
      int idx = c - 'a';
      if (!cur->nodes[idx]) {
        return false;
      }
      cur = cur->nodes[idx];
    }
    return true;
  }

 private:
  struct Node {
    char c;
    bool is_word;
    Node* nodes[26];
    Node(char c = '0', bool is_word = false) : c(c), is_word(is_word) {
      std::memset(nodes, 0, sizeof(nodes));
    }
  };
  Node* head_;
};
}  // namespace

TEST(Leetcode, implement_trie_prefix_tree) {
  Trie trie;
  trie.insert("apple");
  EXPECT_TRUE(trie.search("apple"));
  EXPECT_FALSE(trie.search("app"));
  EXPECT_TRUE(trie.prefix("app"));
  trie.insert("app");
  EXPECT_TRUE(trie.search("apple"));
  EXPECT_TRUE(trie.search("app"));
  EXPECT_TRUE(trie.prefix("app"));
}
