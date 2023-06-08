//=====================================================================
//
// options.cpp -
//
// Created by liubang on 2023/06/01 14:29
// Last Modified: 2023/06/01 14:29
//
//=====================================================================

#include "cpp/misc/sst/options.h"
#include "cpp/misc/sst/comparator.h"
#include "cpp/misc/sst/filter_policy.h"

namespace playground::cpp::misc::sst {

Options::Options()
    : comparator(bytewiseComparator()),
      filter_policy(newBloomFilterPolicy(bits_per_key)) {}

}  // namespace playground::cpp::misc::sst
