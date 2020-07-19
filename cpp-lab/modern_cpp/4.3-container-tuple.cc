#include <iostream>
#include <tuple>

std::tuple<float, char, std::string> get_student(int);

int main(int argc, char* argv[]) {
  auto student = get_student(0);

  std::cout << "ID: 0, "
            << "GPA: " << std::get<0>(student) << ", "
            << "成绩: " << std::get<1>(student) << ", "
            << "姓名: " << std::get<2>(student) << std::endl;

  double gpa;
  char grade;
  std::string name;

  // 拆包
  std::tie(gpa, grade, name) = get_student(1);

  std::cout << "ID: 1, "
            << "GPA: " << gpa << ", "
            << "成绩: " << grade << ", "
            << "姓名: " << name << std::endl;

  // C++14 增加了使用类型来获取元组中的对象：
  auto student1 = get_student(2);
  std::cout << "ID: 2, "
            << "GPA: " << std::get<float>(student1) << ", "
            << "成绩: " << std::get<char>(student1) << ", "
            << "姓名: " << std::get<std::string>(student1) << std::endl;

  return 0;
}

std::tuple<float, char, std::string> get_student(int id) {
  if (id == 0) {
    return std::make_tuple(3.8, 'A', "张三");
  }

  if (id == 1) {
    return std::make_tuple(2.9, 'C', "李四");
  }

  if (id == 2) {
    return std::make_tuple(1.7, 'D', "王五");
  }

  return std::make_tuple(0.0, 'D', "null");
}

