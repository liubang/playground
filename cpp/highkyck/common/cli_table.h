#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace highkyck {
namespace common {

class CliTable {
 public:
  CliTable() {}
  ~CliTable() {}

  void Reset(std::size_t size) {
    cell_size_ = size;
    cell_maxlens_.clear();
    cell_maxlens_.resize(size, 0);
    lines_.clear();
    lines_.resize(1);
  }

  CliTable Add(const std::string& cell) {
    Line& line = lines_.back();
    assert(line.size() < cell_size_);
    line.push_back(cell);
    if (cell.length() > cell_maxlens_[lines_.size() - 1]) {
      cell_maxlens_[lines_.size() - 1] = cell.size();
    }
    return *this;
  }

  // add new line
  CliTable Next() {
    lines_.push_back({});
    return *this;
  }

  void Print() { ::fprintf(stderr, ""); }

 private:
  using Line = std::vector<std::string>;

 private:
  std::size_t cell_size_;
  std::vector<std::size_t> cell_maxlens_;
  std::vector<Line> lines_;
};

}  // namespace common
}  // namespace highkyck
