//=====================================================================
//
// hello_world.cpp -
//
// Created by liubang on 2023/06/19 23:40
// Last Modified: 2023/06/19 23:40
//
//=====================================================================

#include <iostream>

int main(int argc, char *argv[]) {
#pragma omp parallel
  { std::cout << "hello world" << std::endl; }
  return 0;
}
