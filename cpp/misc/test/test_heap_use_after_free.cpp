//=====================================================================
//
// test_heap_use_after_free.cpp -
//
// Created by liubang on 2023/05/22 16:12
// Last Modified: 2023/05/22 16:12
//
//=====================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
auto to_string() -> std::string {
  std::string s;
  uint32_t ip = 168362032;
  uint16_t port = 80;
  uint64_t start_time = 1684743618;
  char ip_port_str[64];
  unsigned char* ip_str = (unsigned char*)(&ip);
  ::memset(ip_port_str, '\0', 64);
  ::snprintf(ip_port_str, 64, "%d.%d.%d.%d:%d:%lu", ip_str[0], ip_str[1], ip_str[2], ip_str[3],
             port, start_time);
  s.assign(ip_port_str);
  return s;
}
}  // namespace

int main(int argc, char* argv[]) {
  /*
   * 这里主要是为了构造heap-use-after-free的场景，
   * ==14709==ERROR: AddressSanitizer: heap-use-after-free on address 0x000102f00c10 at pc
   * 0x0001005f0fcc bp 0x00016fbcdf40 sp 0x00016fbcd6d0
   *
   * 这是一个很典型的场景，也是线上真实发生的例子。主要原因是，to_string()方法返回了一个std::string
   * 的临时对象，而 const char* s是获取了这个临时对象中的c_str()指针，执行完这个赋值之后，
   * std::string临时对象便发生了析构，释放了其内部的raw string，此时，我们在再后面printf中使用这个
   * 字符串指针， 就触发了heap-use-after-free 的问题。
   */
  const char* s = to_string().c_str();

  ::printf("s is %s\n", s);
  ::printf("s is %s\n", s);
  ::printf("s is %s\n", s);
  ::printf("s is %s\n", s);

  return 0;
}
