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
    rows_.clear();
    rows_.resize(1);
  }

  CliTable& Add(const std::string& cell) {
    Row& row = rows_.back();
    assert(row.size() <= cell_size_);
    row.push_back(cell);
    if ((cell.size() + 2) > cell_maxlens_[row.size() - 1]) {
      cell_maxlens_[row.size() - 1] = cell.size() + 2;
    }
    return *this;
  }

  // add new line
  CliTable& Next() {
    rows_.push_back({});
    return *this;
  }

  void Print() {
    for (const auto& row : rows_) {
      PrintLine();
      PrintRow(row);
    }
    PrintLine();
    Reset(0);
  }

 private:
  using Row = std::vector<std::string>;

  void PrintRow(const Row& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      ::fprintf(stdout, "| ");
      ::fprintf(stdout, "%s ", row[i].data());
      if ((row[i].size() + 2) < cell_maxlens_[i]) {
        for (auto n = 0; n < cell_maxlens_[i] - row[i].size() - 2; ++n) {
          ::fprintf(stdout, " ");
        }
      }
    }
    ::fprintf(stdout, "|\n");
  }

  void PrintLine() {
    for (std::size_t i = 0; i < cell_maxlens_.size(); ++i) {
      if (i == 0) ::fprintf(stdout, "+");
      for (std::size_t n = 0; n < cell_maxlens_[i]; ++n) {
        ::fprintf(stdout, "-");
      }
      ::fprintf(stdout, "+");
    }
    ::fprintf(stdout, "\n");
  }

 private:
  std::size_t cell_size_;  // the number of cells per row
  std::vector<std::size_t> cell_maxlens_;
  std::vector<Row> rows_;
};

}  // namespace common
}  // namespace highkyck
