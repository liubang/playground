#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace pl::minivessel {

struct ImmutableObjectIdentity final {
    uint64_t inode_id = 0;
    uint64_t content_generation = 0;
    uint64_t length = 0;
    uint32_t checksum = 0;
    bool checksum_valid = false;
};

class MiniDfsObjectStore final {
public:
    static absl::StatusOr<std::unique_ptr<MiniDfsObjectStore>> Create(std::string namenode_address,
                                                                      std::string client_id,
                                                                      uint32_t replication);
    ~MiniDfsObjectStore();

    absl::StatusOr<ImmutableObjectIdentity> put_immutable(std::string_view path,
                                                          std::string_view content);
    absl::StatusOr<std::string> read(std::string_view path,
                                     const ImmutableObjectIdentity& identity);

private:
    class Impl;
    explicit MiniDfsObjectStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pl::minivessel
