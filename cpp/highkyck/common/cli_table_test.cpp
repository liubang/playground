#include "cli_table.h"

int main(int argc, char* argv[]) {
  highkyck::common::CliTable table;
  table.Reset(3);
  table.Cell("aaaaaa").Cell("bbbbb").Cell("cccc").Next();
  table.Cell("aaaaaa").Cell("bbbbb").Cell("cccc").Next();
  table.Cell("aaaaaa").Cell("bbsaqdbbb").Cell("ccccdfasdfaf").Next();
  table.Cell("aaaaaa").Cell("bbbbllllllllllllllllllb").Cell("cccc").Next();
  table.Cell("adsfakljklwqwerwqaaaaa").Cell("bbbbb").Cell("cccdjalfdwqac");
  table.Print();

  table.Reset(4);
  table.Cell("id").Cell("name").Cell("age").Cell("description").Next();
  table.Cell("0000001").Cell("zhangsan").Cell("20").Cell("this is zhangsan").Next();
  table.Cell("0000002").Cell("lisi").Cell("30").Cell("this is lisi").Next();
  table.Cell("0000003").Cell("wangwu").Cell("25").Cell("this is wangwu").Next();
  table.Print();

  return 0;
}
