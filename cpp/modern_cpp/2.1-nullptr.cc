#include <iostream>
#include <type_traits>

using namespace std;

void foo(char*);
void foo(int);

int main(int argc, char* argv[]) {
  if (is_same<decltype(NULL), decltype(0)>::value)
    cout << "NULL == 0" << endl;

  if (is_same<decltype(NULL), decltype((void*)0)>::value)
    cout << "NULL == (void *)0" << endl;

  if (is_same<decltype(NULL), decltype(nullptr)>::value)
    cout << "NULL == nullptr" << endl;

  foo(0);
  foo(nullptr);

  return 0;
}

void foo(char*) {
  cout << "foo(char *) is called" << endl;
}
void foo(int) {
  cout << "foo(int) is called" << endl;
}
