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
import SwiftUI

/// Top-level connection state: server address + token, health handshake,
/// and the root list store once connected. Address/token persist in
/// UserDefaults; the token is additionally pre-filled from
/// ~/.loom/sessions/serve.token when `loom serve` wrote one.
@MainActor
@Observable
final class AppState {
    enum Status: Equatable {
        case disconnected
        case connecting
        case connected(version: String)
        case failed(String)
    }

    var serverAddress: String {
        didSet { defaults.set(serverAddress, forKey: Self.addressKey) }
    }

    var token: String {
        didSet { defaults.set(token, forKey: Self.tokenKey) }
    }

    private(set) var status: Status = .disconnected
    private(set) var sessionList: SessionListStore?

    /// What the connecting phase is busy with ("Starting loom serve…").
    private(set) var statusDetail: String?

    /// The bundled `loom serve` lifecycle (reuse-or-spawn, kill on quit).
    let server = ServerManager()

    private let defaults = UserDefaults.standard
    private static let addressKey = "loom.serverAddress"
    private static let tokenKey = "loom.token"
    private var didAutoStart = false

    init() {
        serverAddress = defaults.string(forKey: Self.addressKey) ?? ServerManager.defaultAddress
        token = defaults.string(forKey: Self.tokenKey) ?? ServerManager.readServeToken() ?? ""
    }

    /// Automatic startup, called once when the UI first appears: reuse a
    /// healthy `loom serve` or spawn the bundled CLI, then connect. Any
    /// failure lands on ConnectView, where the manual path still works.
    func start() {
        guard !didAutoStart, status == .disconnected else { return }
        didAutoStart = true
        status = .connecting
        statusDetail = "Starting loom serve…"
        Task {
            do {
                let endpoint = try await server.ensureRunning()
                serverAddress = endpoint.address
                if !endpoint.token.isEmpty {
                    token = endpoint.token
                }
            } catch {
                statusDetail = nil
                status = .failed(error.localizedDescription)
                return
            }
            statusDetail = nil
            connect()
        }
    }

    func connect() {
        guard let url = URL(string: serverAddress), url.host != nil else {
            status = .failed("Invalid server address")
            return
        }
        status = .connecting
        let api = APIClient(baseURL: url, token: token.trimmingCharacters(in: .whitespacesAndNewlines))
        Task {
            do {
                let meta = try await api.metaVersion()
                guard meta.protocolField == 1 else {
                    status = .failed("Unsupported protocol version \(meta.protocolField) (client speaks v1)")
                    return
                }
                let list = SessionListStore(api: api)
                sessionList = list
                status = .connected(version: meta.version)
                await list.load()
            } catch {
                status = .failed(error.localizedDescription)
            }
        }
    }

    func disconnect() {
        sessionList = nil
        status = .disconnected
    }

    func refetchTokenFromDisk() {
        if let disk = ServerManager.readServeToken() {
            token = disk
        }
    }
}
