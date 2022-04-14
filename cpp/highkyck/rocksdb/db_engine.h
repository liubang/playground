//=====================================================================
//
// engine.h -
//
// Created by liubang on 2021/01/05 14:34
// Last Modified: 2021/01/05 14:34
//
//=====================================================================
#pragma once

#include <rocksdb/db.h>

#include <memory>
#include <string>

#include "key.h"
#include "val.h"

namespace highkyck {

class DbEngine
{
public:
  explicit DbEngine(const std::string &data_dir, const rocksdb::Options &options);
  virtual ~DbEngine();

  virtual bool open();
  virtual bool close();

  virtual rocksdb::Status set(const Key &key, const Val &val);
  virtual rocksdb::Status get(Val *val, const Key &key);
  virtual rocksdb::Status setx(const Key &key, const Val &val, uint32_t ttl);

private:
  std::string data_dir_;
  std::unique_ptr<rocksdb::DB> db_{ nullptr };
  rocksdb::Options options_;
  rocksdb::WriteOptions write_options_;
  rocksdb::ReadOptions read_options_;
};
}// namespace highkyck
