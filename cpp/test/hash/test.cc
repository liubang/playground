#include <city.h>
#include <iostream>
#include <byteswap.h>
#include <cstdlib>
#include <cstring>

#ifdef WORDS_BIGENDIAN
#  define uint32_in_expected_order(x) (bswap_32(x))
#  define uint64_in_expected_order(x) (bswap_64(x))
#else
#  define uint32_in_expected_order(x) (x)
#  define uint64_in_expected_order(x) (x)
#endif

static uint64 UNALIGNED_LOAD64(const char* p)
{
  uint64 result;
  memcpy(&result, p, sizeof(result));
  return result;
}

static uint32 UNALIGNED_LOAD32(const char* p)
{
  uint32 result;
  memcpy(&result, p, sizeof(result));
  return result;
}

static uint64 HashLen16(uint64 u, uint64 v, uint64 mul)
{
  // Murmur-inspired hashing.
  uint64 a = (u ^ v) * mul;
  a ^= (a >> 47);
  uint64 b = (v ^ a) * mul;
  b ^= (b >> 47);
  b *= mul;
  return b;
}

static uint64 Fetch64(const char* p)
{
  return uint64_in_expected_order(UNALIGNED_LOAD64(p));
}

static uint32 Fetch32(const char* p)
{
  return uint32_in_expected_order(UNALIGNED_LOAD32(p));
}

static uint64 Rotate(uint64 val, int shift)
{
  // Avoid shifting by 64: doing so yields an undefined result.
  return shift == 0 ? val : ((val >> shift) | (val << (64 - shift)));
}

static uint64 ShiftMix(uint64 val)
{
  return val ^ (val >> 47);
}

int main(int argc, char* argv[])
{
  // std::cout << s.size() << std::endl;
  // std::cout << static_cast<int64_t>(CityHash64(s.data(), s.size()))
  //           << std::endl;
  // std::cout << static_cast<int64_t>(Fetch32(s.c_str())) << std::endl;
  for (uint64_t i = 0; i < 10; ++i) {
    std::string s = "helloworldhelloworldhelloworldhelloworldhelloworldhelloworld";
    std::cout << i << " ==> " << static_cast<int64_t>(CityHash64WithSeed(s.data(), s.size(), i))
              << std::endl;
    // std::cout << i << " ==> " << static_cast<int64_t>(bswap_64(i)) <<
    // std::endl;
  }
  return 0;
}
