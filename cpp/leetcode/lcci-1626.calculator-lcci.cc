#include <gtest/gtest.h>

#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
enum class TOKEN_TYPE {
  INT,
  OP,
};

class Operator {
 public:
  Operator() {}
  Operator(char c, int weight) : c_(c), weight_(weight) {}

  int getWeight() const { return weight_; }

  char getOp() const { return c_; }

  bool operator<(const Operator& o) { return weight_ < o.getWeight(); }

  int operator()(int lhs, int rhs) {
    if (c_ == '+') {
      return lhs + rhs;
    } else if (c_ == '-') {
      return lhs - rhs;
    } else if (c_ == '*') {
      return lhs * rhs;
    } else if (c_ == '/') {
      return lhs / rhs;
    }
    return 0;
  }

 private:
  char c_;
  int weight_;
};

class Token {
 public:
  Token(int number) : type_(TOKEN_TYPE::INT), number_(number) {}
  Token(const Operator& op) : type_(TOKEN_TYPE::OP), op_(op) {}

  int getNumber() const { return number_; }

  Operator getOp() const { return op_; }

  TOKEN_TYPE getType() const { return type_; }

 private:
  TOKEN_TYPE type_;
  int number_;
  Operator op_;
};

class Solution {
 public:
  int calculate(const std::string& s) {
    std::vector<Token> tokens;
    rpn(tokens, s);
    std::stack<int> stk;
    for (auto& token : tokens) {
      if (token.getType() == TOKEN_TYPE::INT) {
        stk.push(token.getNumber());
      } else {
        int rhs = stk.top();
        stk.pop();
        int lhs = stk.top();
        stk.pop();
        stk.push(token.getOp()(lhs, rhs));
      }
    }
    return stk.top();
  }

 private:
  // 转为逆波兰表达式
  void rpn(std::vector<Token>& tokens, const std::string& s) {
    std::stack<Operator> stk;
    int len = s.length(), i = 0;
    std::unordered_map<char, int> weights = {
        {'(', 0}, {'+', 1}, {'-', 1}, {'*', 2}, {'/', 2}};
    while (i < len) {
      if (s[i] == ' ') {
        while (i < len && s[i] == ' ') {
          i++;
        }
        if (i >= len) {
          break;
        }
      }
      if (std::isdigit(s[i])) {
        int num = 0;
        while (i < len && std::isdigit(s[i])) {
          num = num * 10 + (s[i++] - '0');
        }
        tokens.emplace_back(num);
        if (i >= len) {
          break;
        }
      } else {
        if (s[i] == ')') {
          while (!stk.empty() && stk.top().getOp() != '(') {
            tokens.emplace_back(stk.top());
            stk.pop();
          }
          if (!stk.empty()) {
            stk.pop();
          }
        } else {
          Operator op(s[i], weights[s[i]]);
          if (s[i] == '(' || stk.empty() || stk.top() < op) {
            stk.push(op);
          } else {
            while (!stk.empty() && !(stk.top() < op)) {
              tokens.emplace_back(stk.top());
              stk.pop();
            }
            stk.push(op);
          }
        }
        i++;
      }
    }
    while (!stk.empty()) {
      tokens.emplace_back(stk.top());
      stk.pop();
    }
  }
};
}  // namespace

TEST(Leetcode, calculator_lcci) {
  Solution s;
  EXPECT_EQ(5, s.calculate(" 3+5 / 2 "));
  EXPECT_EQ(7, s.calculate("3+2*2"));
  EXPECT_EQ(5594,
            s.calculate("2 + 34 - ((56 - 3) * 4 + 2) + (1024 - 543) * 12"));
}
