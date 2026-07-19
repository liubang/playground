// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "braft/raft.h"
#include "cpp/pl/minitable/core/braft_slice_adapter.h"
#include "cpp/pl/minitable/core/data_service_core.h"

namespace pl::minitable {

using ProposalCompletion = std::function<void(absl::StatusOr<ApplyResult>)>;

// Phase-1 mutation proposal boundary. It serializes allocation with Node::apply so
// committed log order cannot regress timestamp metadata, and expected_term fences
// proposals racing with leadership changes. Callbacks may run inline on fast paths.
class SliceProposalFrontend final : public SliceLeadershipObserver {
public:
    SliceProposalFrontend(::braft::Node* node,
                          BraftSliceAdapter* adapter,
                          std::shared_ptr<const codec::CellKeyCodec> codec,
                          uint32_t locality_group_id = 1);
    ~SliceProposalFrontend() override;

    SliceProposalFrontend(const SliceProposalFrontend&) = delete;
    SliceProposalFrontend& operator=(const SliceProposalFrontend&) = delete;

    void propose_put(const proto::v2::PutRequest& request,
                     uint64_t physical_time_ms,
                     ProposalCompletion completion);
    void propose_delete(const proto::v2::DeleteRequest& request,
                        uint64_t physical_time_ms,
                        ProposalCompletion completion);

    void on_slice_leader_start(int64_t term) override;
    void on_slice_leader_stop() override;

    [[nodiscard]] int64_t leader_term() const;
    [[nodiscard]] uint64_t allocated_timestamp_counter() const;

private:
    struct RequestKey {
        std::string client_id;
        std::string request_id;
        auto operator<=>(const RequestKey&) const = default;
    };

    struct InFlightProposal {
        uint64_t payload_hash = 0;
        std::vector<ProposalCompletion> waiters;
    };

    template <typename Request, typename IdentityFn, typename PrepareFn>
    void propose(const Request& request,
                 uint64_t physical_time_ms,
                 ProposalCompletion completion,
                 IdentityFn identity_fn,
                 PrepareFn prepare_fn);

    void finish_proposal(const RequestKey& key, absl::StatusOr<ApplyResult> result);

    ::braft::Node* node_;
    BraftSliceAdapter* adapter_;
    std::shared_ptr<const codec::CellKeyCodec> codec_;
    uint32_t locality_group_id_;

    // submission_mutex_ preserves allocation order through Node::apply. mutex_ protects
    // lifecycle state and is released before any user callback or braft submission.
    std::recursive_mutex submission_mutex_;
    mutable std::mutex mutex_;
    int64_t leader_term_ = -1;
    uint64_t allocated_timestamp_counter_ = 0;
    uint64_t last_physical_time_ms_ = 0;
    std::map<RequestKey, InFlightProposal> in_flight_;
};

} // namespace pl::minitable
