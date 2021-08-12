#include <iostream>
#include <fmt/format.h>

#include "terp.h"

int main(int argc, char* argv[])
{
  basecode::Terp terp(1024 * 1024 * 32);
  basecode::Result result;

  if (!terp.initialize(result)) {
    fmt::print("terp initialize failed.\n");
    return 1;
  }

  auto regs = terp.register_file();
  auto stack_top = regs.sp;
  fmt::print("regs.sp = {:08x}\n", regs.sp);

  terp.push(0x08);
  terp.push(0x04);
  terp.push(0x02);

  fmt::print("regs.sp = {:08x}, number of entries: {}\n",
             regs.sp,
             (stack_top - regs.sp) / sizeof(uint64_t));

  auto t1 = terp.pop();
  auto t2 = terp.pop();
  auto t3 = terp.pop();

  fmt::print("t1 = {:08x}", t1);
  fmt::print("t1 = {:08x}", t2);
  fmt::print("t1 = {:08x}", t3);

  return 0;
}
