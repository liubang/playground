//=====================================================================
//
// db_engine.cc -
//
// Created by liubang on 2021/01/07 18:02
// Last Modified: 2021/01/07 18:02
//
//=====================================================================
#include "db_engine.h"

#include <sstream>

namespace highkyck {

DbEngine::DbEngine(const std::string &data_dir, const rocksdb::Options &options)
  : data_dir_(data_dir), options_(options)
{}

DbEngine::~DbEngine() {}

bool DbEngine::open()
{
  rocksdb::DB *db;
  auto status = rocksdb::DB::Open(options_, data_dir_, &db);
  if (!status.ok()) { return false; }
  db_.reset(db);
  return true;
}

bool DbEngine::close()
{
  if (db_) {
    auto status = db_->Close();
    return status.ok();
  }
  return true;
}

rocksdb::Status DbEngine::set(const Key &key, const Val &val)
{
  if (val.t != Type::STRING_VAL) {
    // TODO
  }
  std::stringstream sbuf;
  msgpack::pack(sbuf, key);
  rocksdb::WriteBatch write_batch;
  rocksdb::Slice slice_key(sbuf.str().data(), sbuf.str().size());
  rocksdb::Slice slice_val(val.v.string_val.data(), val.v.string_val.size());
  write_batch.Put(slice_key, slice_val);
  return db_->Write(write_options_, &write_batch);
}

rocksdb::Status DbEngine::get(Val *val, const Key &key)
{
  std::stringstream sbuf;
  msgpack::pack(sbuf, key);
  rocksdb::Slice slice_key(sbuf.str().data(), sbuf.str().size());
  std::string string_value;
  auto status = db_->Get(read_options_, slice_key, &string_value);
  if (status.ok()) {
    val->v.string_val = std::move(string_value);
    val->t = Type::STRING_VAL;
  }
  return status;
}

}// namespace highkyck
