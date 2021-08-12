#include <string>
#include <vector>
#include <stack>
#include <iostream>
#include <unordered_map>

namespace highkyck {

class ReversePolishNotation
{
public:
  ReversePolishNotation(const std::string& notation)
      : original_(notation)
  {
    generate();
  }

  void print()
  {
    for (auto& line : res_) { std::cout << line; }
    std::cout << std::endl;
  }

private:
  void generate()
  {
    std::unordered_map<char, int> priority = {{'(', 0}, {'+', 1}, {'-', 1}, {'*', 2}, {'/', 2}};
    std::stack<char> stk;
    int len = original_.length(), i = 0;
    while (i < len) {
      if (std::isdigit(original_[i])) {
        std::string num;
        while (i < len && std::isdigit(original_[i])) {
          num.push_back(original_[i]);
          i++;
        }
        res_.emplace_back(std::move(num));
        if (i >= len) { break; }
      }
      if (original_[i] == ')') {
        while (!stk.empty() && stk.top() != '(') {
          res_.emplace_back(1, stk.top());
          stk.pop();
        }
        if (!stk.empty()) { stk.pop(); }
        i++;
      }
      else {
        if (original_[i] == '(' || stk.empty() || priority[stk.top()] < priority[original_[i]]) {
          stk.push(original_[i]);
          i++;
        }
        else {
          while (!stk.empty() && priority[stk.top()] >= priority[original_[i]]) {
            res_.emplace_back(1, stk.top());
            stk.pop();
          }
          stk.push(original_[i]);
          i++;
        }
      }
    }
    while (!stk.empty()) {
      res_.emplace_back(1, stk.top());
      stk.pop();
    }
  }

private:
  std::string original_;
  std::vector<std::string> res_;
};
}   // namespace highkyck

int main(int argc, char* argv[])
{
  highkyck::ReversePolishNotation rpn("9+(3-1)*3+10/2");
  rpn.print();
  return 0;
}
