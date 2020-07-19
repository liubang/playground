#include "common.h"

namespace ds {

void merge(
    std::vector<int>& input,
    const int low,
    const int mid,
    const int high) {
  int i = low;
  int j = mid + 1;
  int k = 0;
  std::vector<int> tmp;
  while (i <= mid && j <= high) {
    if (input[i] < input[j]) {
      tmp.emplace_back(input[i]);
      i++;
    } else {
      tmp.emplace_back(input[j]);
      j++;
    }
  }
  while (i <= mid) {
    tmp.emplace_back(input[i]);
    i++;
  }
  while (j <= high) {
    tmp.emplace_back(input[j]);
    j++;
  }
  assert(tmp.size() == (high - low + 1));
  for (i = low, k = 0; i <= high; i++, k++) {
    input[i] = tmp[k];
  }
}

void mergeSort(std::vector<int>& input, const int low, const int high) {
  if (low >= high) {
    return;
  }
  int mid = low + (high - low) / 2;
  mergeSort(input, low, mid);
  mergeSort(input, mid + 1, high);
  merge(input, low, mid, high);
}

void sort(std::vector<int>& input) {
  mergeSort(input, 0, input.size() - 1);
}

} // namespace ds

TEST(MergeSort, sort) {
  std::vector<int> input = {4, 2, 5, 8, 1, 9};
  ds::sort(input);
  std::vector<int> exp = {1, 2, 4, 5, 8, 9};
  EXPECT_EQ(exp, input);
}
