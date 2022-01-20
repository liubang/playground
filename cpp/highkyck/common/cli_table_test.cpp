#include "cli_table.h"

int main(int argc, char* argv[]) {
  highkyck::common::CliTable table;
  table.Reset(3);

  table.Add("aaaaaa").Add("bbbbb").Add("cccc").Next();
  table.Add("aaaaaa").Add("bbbbb").Add("cccc").Next();
  table.Add("aaaaaa").Add("bbsaqdbbb").Add("ccccdfasdfaf").Next();
  table.Add("aaaaaa").Add("bbbbllllllllllllllllllb").Add("cccc").Next();
  table.Add("adsfakljklwqwerwqaaaaa").Add("bbbbb").Add("cccdjalfdwqac");

  table.Print();

  return 0;
}
