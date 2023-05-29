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

template <typename T, typename = typename std::enable_if<
                          std::is_integral<T>::value && !std::is_same<T, bool>::value, T>::type>
void putInt(std::string* dst, T value, bool use_little_endian = true) {
  if (use_little_endian) {
    value = playground::cpp::tools::Endian::little(value);
  } else {
    value = playground::cpp::tools::Endian::big(value);
  }
  dst->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T, typename = typename std::enable_if<
                          std::is_integral<T>::value && !std::is_same<T, bool>::value, T>::type>
bool getInt(playground::cpp::tools::Binary* input, T* value, bool use_little_endian = true) {
  if (input->size() != sizeof(T)) {
    return false;
  }
  memcpy(value, input->data(), input->size());
  if (use_little_endian) {
    *value = playground::cpp::tools::Endian::little(*value);
  } else {
    *value = playground::cpp::tools::Endian::big(*value);
  }
  return true;
}

}  // namespace playground::cpp::misc::sst
