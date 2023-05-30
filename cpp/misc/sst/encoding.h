//=====================================================================
//
// encoding.h -
//
// Created by liubang on 2023/05/29 23:51
// Last Modified: 2023/05/29 23:51
//
//=====================================================================

#include "cpp/tools/binary.h"
#include "cpp/tools/bits.h"

#include <string>
#include <type_traits>

namespace playground::cpp::misc::sst {

template <typename T, typename std::enable_if<
                          std::is_integral<T>::value && !std::is_same<T, bool>::value, T>::type = 0>
void putInt(std::string *dst, T value) {
  // 这里统一用little endian
  value = playground::cpp::tools::Endian::little(value);
  dst->append(reinterpret_cast<const char *>(&value), sizeof(T));
}

template <typename T, typename std::enable_if<
                          std::is_integral<T>::value && !std::is_same<T, bool>::value, T>::type = 0>
bool getInt(playground::cpp::tools::Binary *input, T *value) {
  if (input->size() != sizeof(T)) {
    return false;
  }
  memcpy(value, input->data(), input->size());
  // 这里统一用little endian
  *value = playground::cpp::tools::Endian::little(*value);
  return true;
}

}  // namespace playground::cpp::misc::sst
