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

/// Owns the embedded `loom` CLI lifecycle.
///
/// `loom serve` is single-instance (flock on the data dir), so startup
/// is a reuse-or-spawn decision: if a healthy server already answers on
/// the default address we attach to it and leave it alone on quit;
/// otherwise we spawn the CLI shipped inside the app bundle, wait for
/// readiness, and terminate that child when the app quits.
@MainActor
@Observable
final class ServerManager {
    enum Mode: Equatable {
        /// Attached to a server we did not start — never killed by us.
        case external
        /// Spawned from the bundled CLI — terminated on app quit.
        case managed(pid: Int32)
    }

    struct Endpoint: Equatable, Sendable {
        let address: String
        let token: String
    }

    enum ServerError: LocalizedError {
        case bundledCLIMissing
        case startFailed(terminationStatus: Int32)

        var errorDescription: String? {
            switch self {
            case .bundledCLIMissing:
                "The bundled loom CLI is missing from this app bundle — start `loom serve` manually and connect below."
            case let .startFailed(status):
                "The bundled loom serve exited before becoming ready (status \(status)). Another loom process may hold the data dir — stop it, or connect manually. Logs: ~/Library/Logs/Loom/server.log"
            }
        }
    }

    static let defaultAddress = "http://127.0.0.1:7680"

    private(set) var mode: Mode?
    private var process: Process?

    /// Reuses a healthy server or spawns the bundled one; returns the
    /// endpoint to connect to.
    func ensureRunning() async throws -> Endpoint {
        if await healthy(address: Self.defaultAddress, token: Self.readServeToken() ?? "") {
            mode = .external
            return Endpoint(address: Self.defaultAddress, token: Self.readServeToken() ?? "")
        }

        guard let cli = Bundle.main.resourceURL?.appendingPathComponent("loom"),
              FileManager.default.isExecutableFile(atPath: cli.path)
        else {
            throw ServerError.bundledCLIMissing
        }

        let process = Process()
        process.executableURL = cli
        process.arguments = ["serve", "--listen", "127.0.0.1:7680"]
        // serve resolves its default workspace from the cwd — LaunchServices
        // starts apps in "/", which is a useless workspace root.
        process.currentDirectoryURL = URL(fileURLWithPath: NSHomeDirectory())
        process.standardOutput = try Self.serverLogHandle()
        process.standardError = process.standardOutput
        self.process = process
        try process.run()
        mode = .managed(pid: process.processIdentifier)

        // Wait for readiness. First-ever launch generates the token file
        // during startup, so re-read it on every poll.
        for _ in 0 ..< 100 {
            if !process.isRunning {
                break
            }
            if let token = Self.readServeToken(),
               await healthy(address: Self.defaultAddress, token: token)
            {
                return Endpoint(address: Self.defaultAddress, token: token)
            }
            try await Task.sleep(for: .milliseconds(150))
        }

        // The child died (e.g. another loom holds the data dir lock) or
        // never came up. One last health check in case an external
        // instance won the race and is actually serving.
        if let token = Self.readServeToken(),
           await healthy(address: Self.defaultAddress, token: token)
        {
            mode = .external
            self.process = nil
            return Endpoint(address: Self.defaultAddress, token: token)
        }
        let status = process.isRunning ? -1 : process.terminationStatus
        self.process = nil
        throw ServerError.startFailed(terminationStatus: status)
    }

    /// Terminates a managed server (SIGTERM → serve's graceful shutdown).
    /// No-op for external instances.
    func stop() {
        guard let process, process.isRunning else { return }
        process.terminate()
    }

    // MARK: - Internals

    /// GET /v1/meta/version — the same handshake AppState.connect() does,
    /// with a short timeout so a dead address fails fast.
    private func healthy(address: String, token: String) async -> Bool {
        guard let url = URL(string: address + "/v1/meta/version") else { return false }
        var request = URLRequest(url: url)
        request.timeoutInterval = 1.5
        if !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        do {
            let (_, response) = try await URLSession.shared.data(for: request)
            return (response as? HTTPURLResponse)?.statusCode == 200
        } catch {
            return false
        }
    }

    /// `loom serve` persists its bearer token to <datadir>/serve.token
    /// (0600) on first launch (SERVE_DESIGN §5.2).
    static func readServeToken() -> String? {
        let path = NSHomeDirectory() + "/.loom/sessions/serve.token"
        guard let raw = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        let token = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return token.isEmpty ? nil : token
    }

    private static func serverLogHandle() throws -> FileHandle {
        let dir = NSHomeDirectory() + "/Library/Logs/Loom"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let path = dir + "/server.log"
        if !FileManager.default.fileExists(atPath: path) {
            FileManager.default.createFile(atPath: path, contents: nil)
        }
        let handle = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        // Truncate first: the handle starts at offset 0 and would
        // otherwise overwrite only the head of the previous run's log,
        // leaving a stale tail glued behind the new content.
        try handle.truncate(atOffset: 0)
        return handle
    }
}
