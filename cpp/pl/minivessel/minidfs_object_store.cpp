#include "cpp/pl/minivessel/minidfs_object_store.h"

#include <utility>

#include "absl/status/status.h"
#include "cpp/pl/minidfs/client/dfs_client.h"

namespace pl::minivessel {

class MiniDfsObjectStore::Impl final {
public:
    explicit Impl(std::unique_ptr<minidfs::DfsClient> dfs_client) : client(std::move(dfs_client)) {}
    std::unique_ptr<minidfs::DfsClient> client;
};

MiniDfsObjectStore::MiniDfsObjectStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MiniDfsObjectStore::~MiniDfsObjectStore() = default;

absl::StatusOr<std::unique_ptr<MiniDfsObjectStore>> MiniDfsObjectStore::Create(
    std::string namenode_address, std::string client_id, uint32_t replication) {
    auto client = minidfs::DfsClient::create({.namenode_address = std::move(namenode_address),
                                              .client_id = std::move(client_id),
                                              .replication = replication});
    if (client == nullptr)
        return absl::UnavailableError("failed to initialize MiniDFS client");
    return std::unique_ptr<MiniDfsObjectStore>(
        new MiniDfsObjectStore(std::make_unique<Impl>(std::move(client))));
}

absl::StatusOr<ImmutableObjectIdentity> MiniDfsObjectStore::put_immutable(
    std::string_view path, std::string_view content) {
    const size_t separator = path.rfind('/');
    if (separator == std::string_view::npos || separator == 0) {
        return absl::InvalidArgumentError("immutable object path must have a parent directory");
    }
    auto created = impl_->client->mkdir(path.substr(0, separator));
    if (created.hasError())
        return absl::UnavailableError(created.error().describe());
    auto stream = impl_->client->create_immutable_output_stream(path, false);
    if (stream.hasError())
        return absl::UnavailableError(stream.error().describe());
    auto written = stream->write(content.data(), content.size());
    if (written.hasError())
        return absl::UnavailableError(written.error().describe());
    auto closed = stream->close();
    if (closed.hasError())
        return absl::UnavailableError(closed.error().describe());
    if (!stream->published_identity().has_value()) {
        return absl::DataLossError("MiniDFS close returned no FileIdentity");
    }
    const auto id = *stream->published_identity();
    return ImmutableObjectIdentity{
        id.inode_id, id.content_generation, id.length, id.checksum, id.checksum_valid};
}

absl::StatusOr<std::string> MiniDfsObjectStore::read(std::string_view path,
                                                     const ImmutableObjectIdentity& identity) {
    minidfs::FileIdentity expected{identity.inode_id,
                                   identity.content_generation,
                                   identity.length,
                                   identity.checksum,
                                   identity.checksum_valid};
    auto result = impl_->client->read_exact(path, 0, identity.length, expected);
    if (result.hasError())
        return absl::DataLossError(result.error().describe());
    return std::move(*result);
}

} // namespace pl::minivessel
