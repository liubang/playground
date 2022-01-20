#pragma once

#include <cassert>
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

  CliTable& Add(const std::string& cell) {
    Line& line = lines_.back();
    assert(line.size() <= cell_size_);
    line.push_back(cell);
    if (cell.length() > cell_maxlens_[line.size() - 1]) {
      cell_maxlens_[line.size() - 1] = cell.size();
    }
    return *this;
  }

  // add new line
  CliTable& Next() {
    lines_.push_back({});
    return *this;
  }

  void Print() {
    for (std::size_t i = 0; i < lines_.size(); ++i) {
      for (std::size_t j = 0; j < cell_maxlens_.size(); ++j) {
        if (j == 0) ::fprintf(stdout, "+");
        for (std::size_t n = 0; n < cell_maxlens_[j]; ++n) {
          ::fprintf(stdout, "-");
        }
        ::fprintf(stdout, "+");
      }
      ::fprintf(stdout, "\n");
      for (std::size_t j = 0; j < lines_[i].size(); ++j) {
        ::fprintf(stdout, "|");
        ::fprintf(stdout, lines_[i][j].data());
        if (lines_[i][j].size() < cell_maxlens_[j]) {
          for (auto n = 0; n < cell_maxlens_[j] - lines_[i][j].size(); ++n) {
            ::fprintf(stdout, " ");
          }
        }
      }
      ::fprintf(stdout, "|\n");
    }
    for (std::size_t j = 0; j < cell_maxlens_.size(); ++j) {
      if (j == 0) ::fprintf(stdout, "+");
      for (std::size_t n = 0; n < cell_maxlens_[j]; ++n) {
        ::fprintf(stdout, "-");
      }
      ::fprintf(stdout, "+");
    }
    ::fprintf(stdout, "\n");
    Reset(0);
  }

 private:
  using Line = std::vector<std::string>;

 private:
  std::size_t cell_size_;
  std::vector<std::size_t> cell_maxlens_;
  std::vector<Line> lines_;
};

}  // namespace common
}  // namespace highkyck
