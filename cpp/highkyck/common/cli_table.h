#pragma once

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace highkyck {
namespace common {

enum class Style {
  None,
  Bold,
};

enum class Color {
  None,
  Red,
  Yellow,
};

class Cell {
 public:
  explicit Cell(const std::string& data)
      : data_(data), style_(Style::None), color_(Color::None) {}

  Cell(const std::string& data, Style style, Color Color)
      : data_(data), style_(style), color_(Color) {}

  const std::string& Data() const { return data_; }

 private:
  const std::string data_;
  Style style_;
  Color color_;
};

class Row {
 public:
  Row() = default;
  ~Row() = default;

  Row& AddCell(std::shared_ptr<Cell> cell) {
    cells_.push_back(cell);
    return *this;
  }

  const std::vector<std::shared_ptr<Cell>>& Cells() const { return cells_; }

 private:
  std::vector<std::shared_ptr<Cell>> cells_;
};

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
      ::fprintf(stdout, "\033[1m%s ", row[i].data());
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
