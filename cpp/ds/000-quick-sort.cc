#include "common.h"

namespace ds {

int partition(std::vector<int>& input, const int l, const int r) {
  int k = l;
  int pivot = input[r];
  for (int i = l; i < r; i++) {
    if (input[i] <= pivot) {
      std::swap(input[i], input[k++]);
    }
  }
  std::swap(input[k], input[r]);
  return k;
}

void quickSort(std::vector<int>& input, const int l, const int r) {
  if (l < r) {
    int p = partition(input, l, r);
    quickSort(input, l, p - 1);
    quickSort(input, p + 1, r);
  }
}
void sort(std::vector<int>& input) {}
} // namespace ds

TEST(QuickSort, sort) {
  std::vector<int> input = {};
}
