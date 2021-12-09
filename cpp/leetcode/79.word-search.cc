#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
class Solution {
 public:
  bool exist(std::vector<std::vector<char>>& board, const std::string& word) {
    for (int i = 0; i < board.size(); ++i) {
      for (int j = 0; j < board[0].size(); ++j) {
        if (dfs(board, word, i, j, 0)) {
          return true;
        }
      }
    }
    return false;
  }

 private:
  bool dfs(std::vector<std::vector<char>>& board, const std::string& word,
           int i, int j, int k) {
    // 条件不满足直接返回
    if (i < 0 || i >= board.size() || j < 0 || j >= board[0].size() ||
        board[i][j] != word[k]) {
      return false;
    }
    // 搜索到word最后一个字符都相等，则找到目标
    if (k == word.length() - 1) {
      return true;
    }
    char tmp = board[i][j];
    // 已经走过的路，暂时将其改变为非字母的字符
    board[i][j] = '#';
    // 上下左右继续搜索
    int dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
    for (int m = 0; m < 4; ++m) {
      int newi = i + dx[m], newj = j + dy[m], newk = k + 1;
      if (dfs(board, word, newi, newj, newk)) {
        return true;
      }
    }
    // 此路不通，退回去
    board[i][j] = tmp;
    return false;
  }
};
}  // namespace

TEST(Leetcode, word_search) {
  Solution s;
  {
    std::vector<std::vector<char>> input = {
        {'A', 'B', 'C', 'E'},
        {'S', 'F', 'C', 'S'},
        {'A', 'D', 'E', 'E'},
    };

    EXPECT_TRUE(s.exist(input, "ABCCED"));
  }

  {
    std::vector<std::vector<char>> input = {
        {'A', 'B', 'C', 'E'},
        {'S', 'F', 'C', 'S'},
        {'A', 'D', 'E', 'E'},
    };
    EXPECT_FALSE(s.exist(input, "ABCB"));
  }
}
