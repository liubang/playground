#include <cassert>
#include <iostream>
#include "rocksdb/db.h"
#include "rocksdb/options.h"

int main(int argc, char* argv[])
{
  rocksdb::DB* db;
  rocksdb::Options options;
  options.create_if_missing = true;
  options.error_if_exists = true;
  // 打开一个数据库
  rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/testdb", &db);

  // 写操作
  auto s = db->Put(rocksdb::WriteOptions(), "test", "OK");
  // 读操作
  if (s.ok()) {
    std::string value;
    s = db->Get(rocksdb::ReadOptions(), "test", &value);
    if (s.ok()) { std::cout << "the value is:" << value << std::endl; }
  }

  delete db;
  return 0;
}
