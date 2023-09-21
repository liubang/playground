//=====================================================================
//
// crc.h -
//
// Created by liubang on 2023/06/01 18:56
// Last Modified: 2023/06/01 18:56
//
//=====================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace pl {

uint32_t crc32(const void* data, std::size_t length);

} // namespace pl
