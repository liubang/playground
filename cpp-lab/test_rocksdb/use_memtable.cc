#include <rocksdb/db.h>
#include <folly/ScopeGuard.h>
#include <folly/Conv.h>
#include <iostream>

int main(int argc, char* argv[]) {
  rocksdb::Options options;
  size_t size = 1024;
  std::shared_ptr<rocksdb::Cache> lrucache =
      rocksdb::NewLRUCache(size, 6, true, 0.5);
  options.write_buffer_manager =
      std::make_shared<rocksdb::WriteBufferManager>(size);
  options.create_if_missing = true;

  rocksdb::DB* db;
  rocksdb::Status status =
      rocksdb::DB::Open(options, "/tmp/memtable_test", &db);

  SCOPE_EXIT {
    delete db;
  };

  if (rocksdb::Status::OK() != status) {
    std::cout << "open db error:" << status.ToString() << std::endl;
    return 1;
  }

  rocksdb::WriteBatch batch;
  for (int i = 0; i < 10000; i++) {
    std::string key = folly::to<std::string>("hello", "_", i);
    std::string val = folly::to<std::string>("world", "_", i);
    auto status = batch.Put(rocksdb::Slice(key), rocksdb::Slice(val));
    if (rocksdb::Status::OK() != status) {
      std::cout << "put error: " << status.ToString() << std::endl;
    }
  }

  if (rocksdb::Status::OK() != db->Write(rocksdb::WriteOptions(), &batch)) {
    std::cout << "write error." << std::endl;
    return 1;
  }

  for (int i = 0; i < 10000; i++) {
    std::string key = folly::to<std::string>("hello", "_", i);
    std::string val;
    if (rocksdb::Status::OK() !=
        db->Get(rocksdb::ReadOptions(), rocksdb::Slice(key), &val)) {
      std::cout << "get error." << std::endl;
    } else {
      std::cout << "key: " << key << ", val: " << val << std::endl;
    }
  }

  return 0;
}
