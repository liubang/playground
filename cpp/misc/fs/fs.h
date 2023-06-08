//=====================================================================
//
// posix_fs_writer.h -
//
// Created by liubang on 2023/05/27 00:34
// Last Modified: 2023/05/27 00:34
//
//=====================================================================

#pragma once

#include "cpp/tools/binary.h"
#include "cpp/tools/status.h"
namespace playground::cpp::misc::fs {

class FsReader;
class FsWriter;

class Fs {
 public:
  Fs();
  Fs(const Fs&) = delete;
  Fs& operator=(const Fs&) = delete;

  virtual ~Fs();

  static Fs* getInstance();

  virtual tools::Status newFsReader(const std::string& filename,
                                    FsReader** result) = 0;
  virtual tools::Status newFsWriter(const std::string& filename,
                                    FsWriter** result) = 0;
};

class FsReader {
 public:
  virtual ~FsReader();

  virtual tools::Status read(uint64_t offset,
                             size_t n,
                             tools::Binary* result,
                             char* scratch) const = 0;

  [[nodiscard]] virtual std::size_t size() const = 0;
};

class FsWriter {
 public:
  FsWriter() = default;
  FsWriter(const FsWriter&) = delete;
  FsWriter& operator=(const FsWriter&) = delete;
  virtual ~FsWriter();
  virtual tools::Status append(const tools::Binary& data) = 0;
  virtual tools::Status close() = 0;
  virtual tools::Status flush() = 0;
  virtual tools::Status sync() = 0;
};

}  // namespace playground::cpp::misc::fs
