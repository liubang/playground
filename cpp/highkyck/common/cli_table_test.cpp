#include "cli_table.h"

#include <gtest/gtest.h>

TEST(common, cli_table) {
  highkyck::common::CliTable table;
  table.Reset(3);
  table.Cell("aaaaaa").Cell("bbbbb").Cell("cccc").NewRow();
  table.Cell("aaaaaa").Cell("bbbbb").Cell("cccc").NewRow();
  table.Cell("aaaaaa").Cell("bbsaqdbbb").Cell("ccccdfasdfaf").NewRow();
  table.Cell("aaaaaa").Cell("bbbbllllllllllllllllllb").Cell("cccc").NewRow();
  table.Cell("adsfakljklwqwerwqaaaaa").Cell("bbbbb").Cell("cccdjalfdwqac");
  table.Print();

  table.Reset(4);
  table.Cell("id")
      .Cell("name")
      .Cell("age")
      .Cell("description")
      .NewRow()
      .Cell("0000001")
      .Cell("zhangsan")
      .Cell("20")
      .Cell("this is zhangsan")
      .NewRow()
      .Cell("0000002")
      .Cell("lisi")
      .Cell("30")
      .Cell("this is lisi")
      .NewRow()
      .Cell("0000003")
      .Cell("wangwu")
      .Cell("25")
      .Cell("this is wangwu")
      .NewRow()
      .Cell("0000004")
      .Cell("zhangsan")
      .Cell("22")
      .Cell("this is zhangsan")
      .Print();
}
