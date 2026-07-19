// Copyright (c) 2026 The Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
#include "cpp/pl/minitable/core/slice_proposal_frontend.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "butil/iobuf.h"

namespace pl::minitable {
namespace {

struct ProposalOutcome {
    std::mutex mutex;
    std::optional<ApplyResult> result;
};

class ProposalClosure final : public ::braft::Closure {
public:
    ProposalClosure(BraftSliceAdapter* adapter,
                    std::shared_ptr<ProposalOutcome> outcome,
                    ProposalCompletion completion)
        : adapter_(adapter), outcome_(std::move(outcome)), completion_(std::move(completion)) {}

    void Run() override {
        std::unique_ptr<ProposalClosure> self(this);
        adapter_->unregister_proposal(this);
        if (!status().ok()) {
            completion_(absl::UnavailableError(status().error_str()));
            return;
        }
        std::optional<ApplyResult> result;
        {
            std::lock_guard lock(outcome_->mutex);
            result = std::move(outcome_->result);
        }
        if (!result.has_value()) {
            completion_(absl::InternalError("successful proposal completed without ApplyResult"));
            return;
        }
        completion_(std::move(*result));
    }

private:
    BraftSliceAdapter* adapter_;
    std::shared_ptr<ProposalOutcome> outcome_;
    ProposalCompletion completion_;
};

} // namespace

SliceProposalFrontend::SliceProposalFrontend(::braft::Node* node,
                                             BraftSliceAdapter* adapter,
                                             std::shared_ptr<const codec::CellKeyCodec> codec,
                                             uint32_t locality_group_id)
    : node_(node),
      adapter_(adapter),
      codec_(std::move(codec)),
      locality_group_id_(locality_group_id) {
    adapter_->set_leadership_observer(this);
}

SliceProposalFrontend::~SliceProposalFrontend() {
    adapter_->set_leadership_observer(nullptr);
}

void SliceProposalFrontend::propose_put(const proto::v2::PutRequest& request,
                                        uint64_t physical_time_ms,
                                        ProposalCompletion completion) {
    propose(request,
            physical_time_ms,
            std::move(completion),
            PutIdentityV2,
            [](const auto& value,
               CommitAllocation allocation,
               uint32_t locality_group_id,
               const auto& codec) {
                return PreparePutV2(value, allocation, locality_group_id, codec);
            });
}

void SliceProposalFrontend::propose_delete(const proto::v2::DeleteRequest& request,
                                           uint64_t physical_time_ms,
                                           ProposalCompletion completion) {
    propose(request,
            physical_time_ms,
            std::move(completion),
            DeleteIdentityV2,
            [](const auto& value,
               CommitAllocation allocation,
               uint32_t locality_group_id,
               const auto& codec) {
                return PrepareDeleteV2(value, allocation, locality_group_id, codec);
            });
}

template <typename Request, typename IdentityFn, typename PrepareFn>
void SliceProposalFrontend::propose(const Request& request,
                                    uint64_t physical_time_ms,
                                    ProposalCompletion completion,
                                    IdentityFn identity_fn,
                                    PrepareFn prepare_fn) {
    auto identity = identity_fn(request);
    if (!identity.ok()) {
        completion(identity.status());
        return;
    }

    const RequestKey request_key{.client_id = identity->client_id,
                                 .request_id = identity->request_id};
    std::lock_guard submission_lock(submission_mutex_);
    std::unique_lock state_lock(mutex_);
    if (leader_term_ <= 0) {
        state_lock.unlock();
        completion(absl::FailedPreconditionError("Slice replica is not leader"));
        return;
    }
    const auto dedupe = adapter_->state_machine().machine().lookup_dedupe(*identity);
    if (dedupe.kind == DedupeLookupKind::kDuplicate) {
        state_lock.unlock();
        completion(
            ApplyResult{.duplicate = true, .serialized_response = dedupe.serialized_response});
        return;
    }
    if (dedupe.kind == DedupeLookupKind::kConflict) {
        state_lock.unlock();
        completion(absl::AlreadyExistsError("mutation identity was reused with another payload"));
        return;
    }
    const auto in_flight = in_flight_.find(request_key);
    if (in_flight != in_flight_.end()) {
        if (in_flight->second.payload_hash != identity->payload_hash) {
            state_lock.unlock();
            completion(absl::AlreadyExistsError(
                "in-flight mutation identity was reused with another payload"));
        } else {
            in_flight->second.waiters.push_back(std::move(completion));
        }
        return;
    }
    if (physical_time_ms == 0) {
        state_lock.unlock();
        completion(absl::InvalidArgumentError("physical commit time must be non-zero"));
        return;
    }
    if (allocated_timestamp_counter_ == std::numeric_limits<uint64_t>::max()) {
        state_lock.unlock();
        completion(absl::ResourceExhaustedError("Slice-local timestamp counter is exhausted"));
        return;
    }

    const CommitAllocation allocation{
        .commit_ts =
            {.domain_epoch =
                 adapter_->state_machine().machine().store().persistence().timestamp_domain_epoch,
             .counter = allocated_timestamp_counter_ + 1},
        .commit_physical_ms = std::max(physical_time_ms, last_physical_time_ms_),
    };
    auto prepared = prepare_fn(request, allocation, locality_group_id_, *codec_);
    if (!prepared.ok()) {
        state_lock.unlock();
        completion(prepared.status());
        return;
    }
    allocated_timestamp_counter_ = allocation.commit_ts.counter;
    last_physical_time_ms_ = allocation.commit_physical_ms;
    in_flight_.emplace(request_key,
                       InFlightProposal{.payload_hash = identity->payload_hash,
                                        .waiters = {std::move(completion)}});

    auto outcome = std::make_shared<ProposalOutcome>();
    auto* closure = new ProposalClosure(
        adapter_, outcome, [this, request_key](absl::StatusOr<ApplyResult> result) mutable {
            finish_proposal(request_key, std::move(result));
        });
    adapter_->register_proposal(closure, [outcome](ApplyResult result) {
        std::lock_guard outcome_lock(outcome->mutex);
        outcome->result = std::move(result);
    });
    const int64_t expected_term = leader_term_;
    state_lock.unlock();

    butil::IOBuf data;
    data.append(prepared->encoded_entry);
    ::braft::Task task;
    task.data = &data;
    task.done = closure;
    task.expected_term = expected_term;
    node_->apply(task);
}

void SliceProposalFrontend::finish_proposal(const RequestKey& key,
                                            absl::StatusOr<ApplyResult> result) {
    std::vector<ProposalCompletion> waiters;
    {
        std::lock_guard lock(mutex_);
        const auto it = in_flight_.find(key);
        if (it == in_flight_.end()) {
            return;
        }
        waiters = std::move(it->second.waiters);
        in_flight_.erase(it);
    }
    for (auto& waiter : waiters) {
        if (result.ok()) {
            waiter(*result);
        } else {
            waiter(result.status());
        }
    }
}

void SliceProposalFrontend::on_slice_leader_start(int64_t term) {
    std::lock_guard lock(mutex_);
    const auto& store = adapter_->state_machine().machine().store();
    allocated_timestamp_counter_ =
        std::max(allocated_timestamp_counter_, store.timestamp_high_watermark());
    last_physical_time_ms_ = std::max(last_physical_time_ms_, store.last_commit_physical_ms());
    leader_term_ = term;
}

void SliceProposalFrontend::on_slice_leader_stop() {
    std::lock_guard lock(mutex_);
    leader_term_ = -1;
}

int64_t SliceProposalFrontend::leader_term() const {
    std::lock_guard lock(mutex_);
    return leader_term_;
}

uint64_t SliceProposalFrontend::allocated_timestamp_counter() const {
    std::lock_guard lock(mutex_);
    return allocated_timestamp_counter_;
}

} // namespace pl::minitable
