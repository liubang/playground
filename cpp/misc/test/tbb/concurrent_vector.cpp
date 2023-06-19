//=====================================================================
//
// concurrent_vector.cpp -
//
// Created by liubang on 2023/06/18 14:35
// Last Modified: 2023/06/18 14:35
//
//=====================================================================
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <iostream>

#include "cpp/tools/measure.h"

int main(int argc, char *argv[]) {
  size_t n = 1 << 27;

  pl::measure([n] {
    tbb::concurrent_vector<float> a;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n),
                      [&](tbb::blocked_range<size_t> r) {
                        for (size_t i = r.begin(); i < r.end(); ++i) {
                          float val = std::sin(i);
                          if (val > 0) a.push_back(val);
                        }
                      });
  });

  pl::measure([n] {
    tbb::concurrent_vector<float> a;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n),
                      [&](tbb::blocked_range<size_t> r) {
                        std::vector<float> local_a;
                        for (size_t i = r.begin(); i < r.end(); ++i) {
                          float val = std::sin(i);
                          if (val > 0) local_a.push_back(val);
                        }

                        auto it = a.grow_by(local_a.size());
                        std::copy(local_a.begin(), local_a.end(), it);
                      });
  });

  return 0;
}
