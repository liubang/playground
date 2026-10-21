// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import Foundation
import Observation

/// Shared projection state for the trace strip and execution maze.
/// Refresh intents coalesce behind one load; activity arriving in flight
/// guarantees a follow-up, even after the run idles.
@MainActor
@Observable
final class MazeStore {
    private(set) var data: MazeData?
    private(set) var error: String?
    private(set) var fetchedAt: Date?
    private(set) var generation = 0

    @ObservationIgnored private let load: @Sendable () async throws -> MazeData
    @ObservationIgnored private var worker: Task<Void, Never>?
    @ObservationIgnored private var lifecycle = 0
    @ObservationIgnored private var active = true

    init(load: @escaping @Sendable () async throws -> MazeData) {
        self.load = load
    }

    /// Re-entering a tab keeps its last successful snapshot until refreshed.
    @discardableResult
    func activate() -> Int {
        cancel()
        active = true
        return lifecycle
    }

    /// The worker is not a child of an event debounce task: cancelling a
    /// debounce must not repeatedly abort network requests and starve the UI.
    @discardableResult
    func requestRefresh() -> Task<Void, Never>? {
        guard active, !Task.isCancelled else { return nil }
        generation += 1
        if let worker {
            return worker
        }
        let lifecycle = lifecycle
        let load = load
        worker = Task { [weak self] in
            defer {
                if let self, self.lifecycle == lifecycle {
                    self.worker = nil
                }
            }
            while !Task.isCancelled {
                guard let requested = self?.requestedGeneration(in: lifecycle) else { return }
                let result: Result<MazeData, Error>
                do {
                    result = try await .success(load())
                } catch {
                    result = .failure(error)
                }
                guard !Task.isCancelled, let self,
                      active, self.lifecycle == lifecycle else { return }
                // Loads within a lifecycle are serial: an accepted result
                // cannot overwrite a newer one. Publish progress even during
                // continuous activity, then catch up with one coalesced load.
                switch result {
                case let .success(fresh):
                    data = fresh
                    error = nil
                    fetchedAt = Date()
                case let .failure(error):
                    // URLSession may report cancellation as URLError rather
                    // than CancellationError. Neither is a projection error.
                    if !(error is CancellationError),
                       (error as? URLError)?.code != .cancelled
                    {
                        self.error = error.localizedDescription
                    }
                }
                if requested != generation {
                    continue
                }
                worker = nil
                return
            }
        }
        return worker
    }

    private func requestedGeneration(in lifecycle: Int) -> Int? {
        guard active, self.lifecycle == lifecycle else { return nil }
        return generation
    }

    /// Invalidate before cancelling: even a source that ignores cancellation
    /// cannot mutate state or clear a newer worker when it eventually returns.
    func cancel(lifecycle expected: Int? = nil) {
        if let expected, expected != lifecycle {
            return
        }
        active = false
        lifecycle += 1
        generation += 1
        worker?.cancel()
        worker = nil
    }

    deinit {
        worker?.cancel()
    }
}
