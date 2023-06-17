//=====================================================================
//
// crc.cpp -
//
// Created by liubang on 2023/06/01 18:57
// Last Modified: 2023/06/01 18:57
//
//=====================================================================

#include "cpp/tools/crc.h"

#include <boost/crc.hpp>

namespace pl {

uint32_t crc32(const void* data, std::size_t length) {
  boost::crc_32_type result;
  result.process_bytes(data, length);
  return result.checksum();
}

}  // namespace pl
