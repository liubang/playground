// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/braft_slice_adapter.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "braft/storage.h"
#include "brpc/closure_guard.h"
#include "butil/iobuf.h"
namespace pl::minitable {

void BraftSliceAdapter::on_apply(::braft::Iterator& iterator) {
    for (; iterator.valid(); iterator.next()) {
        ::brpc::ClosureGuard done_guard(iterator.done());
        std::string encoded;
        iterator.data().copy_to(&encoded);
        auto result = state_machine_->on_braft_apply(
            static_cast<uint64_t>(iterator.index()),
            static_cast<uint64_t>(iterator.term()),
            std::as_bytes(std::span(encoded.data(), encoded.size())));
        if (!result.ok()) {
            // braft owns and fails the current/tail closures after rollback. Do not run the
            // current closure here, otherwise a proposal may observe two inconsistent callbacks.
            done_guard.release();
            butil::Status failure;
            failure.set_error(EINVAL, "%s", result.status().ToString().c_str());
            iterator.set_error_and_rollback(1, &failure);
            return;
        }
    }
}

void BraftSliceAdapter::on_snapshot_save(::braft::SnapshotWriter* writer,
                                         ::braft::Closure* done) {
    ::brpc::ClosureGuard done_guard(done);
    auto encoded = state_machine_->on_snapshot_save();
    if (!encoded.ok()) {
        done->status().set_error(EIO, "%s", encoded.status().ToString().c_str());
        return;
    }
    const std::string path = writer->get_path() + "/" + kSnapshotFile;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
    output.close();
    if (!output || writer->add_file(kSnapshotFile) != 0) {
        done->status().set_error(EIO, "failed to publish SliceSnapshot");
    }
}

int BraftSliceAdapter::on_snapshot_load(::braft::SnapshotReader* reader) {
    if (reader->get_file_meta(kSnapshotFile, nullptr) != 0) {
        std::fprintf(stderr, "SliceSnapshot metadata is missing from braft SnapshotReader\n");
        return -1;
    }
    const std::string path = reader->get_path() + "/" + kSnapshotFile;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        std::fprintf(stderr, "failed to open SliceSnapshot payload at %s\n", path.c_str());
        return -1;
    }
    std::string encoded((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        std::fprintf(stderr, "failed to read SliceSnapshot payload from %s\n", path.c_str());
        return -1;
    }
    auto status = state_machine_->on_snapshot_load(
        std::as_bytes(std::span(encoded.data(), encoded.size())), locality_groups_, persistence_);
    if (!status.ok()) {
        std::fprintf(stderr, "failed to install SliceSnapshot: %s\n", status.ToString().c_str());
        return -1;
    }
    return 0;
}

} // namespace pl::minitable
