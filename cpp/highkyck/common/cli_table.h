#pragma once

#include <cassert>
#include <codecvt>
#include <cstdio>
#include <locale>
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

  CliTable& Cell(const std::string& cell) {
    Row& row = rows_.back();
    assert(row.size() <= cell_size_);
    row.push_back(cell);
    if ((cell.length() + 2) > cell_maxlens_[row.size() - 1]) {
      cell_maxlens_[row.size() - 1] = cell.length() + 2;
    }
    return *this;
  }

  // add new line
  CliTable& NewRow() {
    rows_.push_back({});
    return *this;
  }

  void Print() {
    PrintLine();
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      PrintRow(rows_[i]);
      if (i == 0) {
        PrintLine();
      }
    }
    PrintLine();
    Reset(0);
  }

 private:
  using Row = std::vector<std::string>;

  void PrintRow(const Row& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      fprintf(stdout, "| %s ", row[i].data());
      if ((row[i].length() + 2) < cell_maxlens_[i]) {
        const std::string data(cell_maxlens_[i] - row[i].length() - 2, ' ');
        fprintf(stdout, "%s", data.data());
      }
    }
    fprintf(stdout, "|\n");
  }

  void PrintLine() {
    for (std::size_t i = 0; i < cell_maxlens_.size(); ++i) {
      const std::string data(cell_maxlens_[i], '-');
      fprintf(stdout, "+%s", data.data());
    }
    fprintf(stdout, "+\n");
  }

  // std::wstring ToWstring(const std::string& str) {
  //   std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
  //   return convert.from_bytes(str);
  // }

 private:
  std::size_t cell_size_;  // the number of cells per row
  std::vector<std::size_t> cell_maxlens_;
  std::vector<Row> rows_;
};

}  // namespace common
}  // namespace highkyck
