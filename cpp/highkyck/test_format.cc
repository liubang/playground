#include <iostream>
#include <string>

#define AAAA       "aaaa"
#define BBB        "bbb"
#define BBBBBBBBBB "bbb"

#define ccc "bbb"

#ifdef __linux__
# define FOO
#else
# define BAR
#endif

namespace highkyck {
namespace misk {

template<typename T>
T add(const T& a, const T& b)
{
  return a + b;
}

class bar
{
public:
  bar(int a, int b, int ccccccccccccccccccccccc, int dddddddddddddddddd, int ffffffffffffffffffffff,
      int eeeeeeeeeeeeeeeeeeeeeeee, int f, const std::string& ggg){};

  virtual ~bar() = default;

  //// BinPackParameters: false
  // void print(int                a,
  //            int                b,
  //            int                ccccccccccccccccccccccc,
  //            int                dddddddddddddddddd,
  //            int                ffffffffffffffffffffff,
  //            int                eeeeeeeeeeeeeeeeeeeeeeee,
  //            int                f,
  //            const std::string& ggg);

  void p(int a, int b, int ccccccccccccccccccccccc, int dddddddddddddddddd,
         int ffffffffffffffffffffff, int eeeeeeeeeeeeeeeeeeeeeeee, int f, const std::string& ggg);

private:
  std::string str_;
  std::size_t size_;
  int b_;
};

void foo(int a)
{

  bar b(1, 1, 1, 1, 1, 1111111111111, 1111111111111,
        "dlkfjkldsfjdkls;afjkdalsfjdkslafjdlkslda;df;kd;");

  b.p(1, 1, 1, 1, 1, 1111111111111, 1111111111111,
      "dlkfjkldsfjdkls;afjkdalsfjdkslafjdlkslda;df;kd;");

  if (a) { std::cout << "OK"; }

  if (a) {
    std::cout << "OK";
  } else {
    std::cout << "no";
  }

  for (;;) { std::cout << "ok"; }

  if (a) {
    std::cout << "OK";
  } else {
    std::cout << "No";
  }

  switch (a) {
  case 1:
  {
    std::cout << "OK";
    std::cout << "OK";
  } break;
  case 2: std::cout << "ok"; break;
  }

  const std::string s = "aaaaa"
                        "bbbb"
                        "ccccc"
                        "ddddddddddd";

  const std::string ss = "adfsasklfjlasjfkweruieolsjfkds;ajf;kfjkdlasfjkdsal;"
                         "kfjkdlsajfkld;sakjfldasfkdsafj";

  const char* x = "veryVeryVeryVeryVeryVeryVeryVeryVeryVeryVeryVeryLongStringve"
                  "ryVeryVeryVeryVeryVe"
                  "ryVeryVeryVeryVeryVeryVeryLongString";
}

void foo1(int a)
{

  if (a) {
    goto end;
  } else {
    goto start;
  }

start:
  std::cout << "start";

end:
  std::cout << "endl";
}
}  // namespace misk
}  // namespace highkyck
