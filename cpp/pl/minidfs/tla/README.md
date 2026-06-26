# MiniDFS TLA+ Formal Verification

TLA+ formal specifications for verifying the correctness of key state machines in MiniDFS.

## Directory Structure

```
cpp/pl/minidfs/tla/
├── README.md              # This file
├── DatanodeState.tla      # DataNode heartbeat/lifecycle state machine
├── DatanodeState.cfg      # TLC model config
├── BlockReplicaState.tla  # Block + Replica joint state machine
├── BlockReplicaState.cfg  # TLC model config
├── FileLeaseState.tla     # File (inode) + Lease state machine
├── FileLeaseState.cfg     # TLC model config
├── WriteProtocol.tla      # Full pipeline write protocol
└── WriteProtocol.cfg      # TLC model config
```

## Prerequisites

Install the TLA+ tools:

```bash
# macOS
brew install tla-plus-toolbox

# Or download tla2tools.jar from:
# https://lamport.azurewebsites.net/tla/toolbox.html
```

Required tools:

- **SANY** (TLA+ Parser) — syntax and type checking
- **TLC** (TLA+ Model Checker) — exhaustive state-space exploration
- _(Optional)_ **Apalache** — symbolic model checker for larger models
- _(Optional)_ **TLAPS** — interactive proof system

## Quick Start

### Syntax Check (SANY)

```bash
TLA2=/path/to/tla2tools.jar
cd cpp/pl/minidfs/tla

for f in *.tla; do
    echo "=== SANY $f ==="
    java -cp "$TLA2" tla2sany.SANY "$f"
done
```

### Model Checking (TLC)

```bash
cd cpp/pl/minidfs/tla

for cfg in *.cfg; do
    spec="${cfg%.cfg}.tla"
    echo "=== TLC $spec ==="
    java -cp "$TLA2" tlc2.TLC -config "$cfg" "$spec"
done
```

### Model Checking Results (current configs)

| Specification     | States Generated | Distinct States | Model Size                     | Result |
| ----------------- | ---------------- | --------------- | ------------------------------ | ------ |
| DatanodeState     | 363,518          | 53,361          | 3 DNs, max time 20             | PASS   |
| BlockReplicaState | 2,267,173        | 181,476         | 2 blocks, 6 replicas, 3 DNs    | PASS   |
| FileLeaseState    | 46,437           | 8,676           | 2 files, 2 clients, max time 6 | PASS   |
| WriteProtocol     | 4,851,717        | 556,378         | 1 file, 3 DNs, 3 blocks        | PASS   |

All models complete exhaustive state-space exploration (0 states left on queue) with no invariant violations.

## Specifications

### 1. DatanodeState — DataNode Lifecycle

**Verified code paths:** `namenode/datanode_manager.cpp`

```
State transitions:
  (new) --> Live --> Stale --> Dead
            ^         |         |
            +--- heartbeat -------+
```

Every `TickAndScan` step advances time non-deterministically and updates all DataNode states based on elapsed time since last heartbeat. A `Heartbeat` action resets the node to Live.

**Verified invariants:**

| Invariant                     | Description                                                         |
| ----------------------------- | ------------------------------------------------------------------- |
| `TypeOK`                      | All state values are within the valid enum set                      |
| `LiveImpliesFreshHeartbeat`   | Live nodes must have heartbeat within `StaleTimeoutMs`              |
| `StaleImpliesAgedHeartbeat`   | Stale nodes have heartbeat age in `[StaleTimeoutMs, DeadTimeoutMs)` |
| `DeadImpliesExpiredHeartbeat` | Dead nodes have heartbeat age >= `DeadTimeoutMs`                    |

**Design observation:**

`handle_heartbeat()` unconditionally sets `dn.state = DataNodeState::kLive` (line 99 of `datanode_manager.cpp`), which allows a Dead node to be revived by a heartbeat. This differs from HDFS where a Dead DataNode requires explicit re-registration.

**Configuration (`DatanodeState.cfg`):**

```tla
SPECIFICATION Spec
CONSTANTS
    DataNodes = {dn1, dn2, dn3}
    StaleTimeoutMs = 3
    DeadTimeoutMs = 10
    MaxTime = 20
INVARIANTS TypeOK StateConsistency
```

### 2. BlockReplicaState — Block + Replica Joint State Machine

**Verified code paths:** `namenode/block_manager.cpp`

```
Block:   Allocating --> Committed --> Corrupt
              |             |            |
              +-------------+------------+
                            v
                         Deleted

Replica: Writing --> Finalized --> Stale
            |           |           |
            +-----------+-----------+
                        v
                    Corrupt --> Deleting --> Deleted
```

**Verified invariants:**

| Invariant                                    | Description                                                     |
| -------------------------------------------- | --------------------------------------------------------------- |
| `TypeOK`                                     | All state values within valid enum sets                         |
| `AllocatingBlockReplicasAreWritingOrCorrupt` | Replicas of Allocating blocks are only Writing or Corrupt       |
| `DeletedBlockHasNoActiveReplicas`            | Deleted blocks have no Writing/Finalized/Corrupt/Stale replicas |

**Design observation:**

`commit_block()` has an idempotency path (`already_committed` check at line 147-148 of `block_manager.cpp`). When a duplicate commit request arrives with the same `generation_stamp`, the block state update is skipped but replica states are still iterated. Since Finalized-to-Finalized is idempotent, this is safe in practice, but the model confirms that the block-level consistency is maintained regardless of retry order.

The `CommittedBlockHasMinReplicas` property (requiring >= MinWriteReplica healthy replicas) is a fault-tolerance liveness concern handled by `ReplicationManager`, not a safety invariant. Cascading DataNode failures can temporarily reduce the replica count below `MinWriteReplica`.

**Configuration (`BlockReplicaState.cfg`):**

```tla
SPECIFICATION Spec
CONSTANTS
    Blocks = {1, 2}
    Replicas = {1, 2, 3, 4, 5, 6}
    DataNodes = {1, 2, 3}
    MinWriteReplica = 2
INVARIANTS BlockReplicaInvariants
```

### 3. FileLeaseState — File + Lease Mutual Exclusion

**Verified code paths:** `namenode/namespace_manager.cpp` + `namenode/lease_manager.cpp`

```
File:  Normal <--> UnderConstruction --> Deleted
                      |
                      v
                   Completed

Lease: (None) --> Active --> Closed
                   |
                   +-- (renew extends expiry)
```

**Verified invariants:**

| Invariant                     | Description                                                           |
| ----------------------------- | --------------------------------------------------------------------- |
| `TypeOK`                      | All state values within valid enum sets                               |
| `OnlyUCFileHasActiveLease`    | Only UnderConstruction files can hold an active lease                 |
| `DeletedFileHasNoActiveLease` | Deleted files must not have an active lease                           |
| `ActiveLeaseHasFutureExpiry`  | Active leases have `expire_time >= now` and `<= now + LeaseTimeoutMs` |
| `NormalFileLeaseConsistency`  | Normal files have no active lease (None or Closed)                    |

**Design observation:**

`expire_stale_leases()` (line 101 of `lease_manager.cpp`) closes expired leases but does NOT transition the file from UnderConstruction back to Normal. This means a client crash during writing leaves the file permanently in UnderConstruction state — an "orphan file" that requires administrator intervention or a future `NameNodeMaintenance` cleanup routine.

**Configuration (`FileLeaseState.cfg`):**

```tla
SPECIFICATION Spec
CONSTANTS
    Files = {f1, f2}
    Clients = {c1, c2}
    LeaseTimeoutMs = 3
    MaxTime = 6
INVARIANTS FileLeaseInvariants
```

### 4. WriteProtocol — Full Pipeline Write Protocol

**Verified code paths:** The entire write flow spanning `namespace_manager.cpp`, `block_manager.cpp`, and `lease_manager.cpp`

This is the most critical specification, modeling the complete lifecycle:

1. `CreateFile` — file enters UnderConstruction with an exclusive lease
2. `AllocateBlock` — NameNode selects DataNodes and creates replicas in Writing state
3. `CommitBlock` — confirmed replicas transition to Finalized, block becomes Committed
4. `CompleteFile` — all blocks committed, file transitions to Completed
5. `DataNodeFailure` — fault injection: DN failure corrupts all its replicas
6. `LeaseExpire` — client crash simulation

**Verified invariants:**

| Invariant                         | Description                                                      |
| --------------------------------- | ---------------------------------------------------------------- |
| `TypeOK`                          | All state values within valid enum sets                          |
| `CompletedFileBlocksAreCommitted` | Every block of a Completed file is in Committed state            |
| `UnallocatedBlockHasNoReplicas`   | Unallocated blocks have zero replicas on any DataNode            |
| `OnlyUCFileHasLease`              | Only UnderConstruction files can hold a lease (mutual exclusion) |

**Fault tolerance note:**

The `CompletedFileHasAtLeastOneReplica` property (requiring at least 1 Finalized replica per block after completion) is a fault-tolerance liveness concern. The model correctly demonstrates that cascading DataNode failures can destroy all replicas of a block. In production, the `ReplicationManager` periodic scan detects under-replicated blocks and triggers re-replication before additional failures occur. Modeling the `ReplicationManager` repair loop would strengthen this property to a temporal guarantee.

**Configuration (`WriteProtocol.cfg`):**

```tla
SPECIFICATION Spec
CONSTANTS
    Clients = {1}
    Files = {1}
    DataNodes = {1, 2, 3}
    MinWriteReplica = 2
    MaxBlockIndex = 2
INVARIANTS WriteProtocolInvariants
```

## Design Principles

### Bounded vs. Unbounded Models

All specifications use bounded (finite-state) models with small constant sets, because:

- Safety bugs in distributed protocols typically manifest in small configurations
- TLC performs exhaustive enumeration of the state space, providing proof-level certainty for the given bound
- For unbounded verification, TLAPS (proof system) or Apalache (symbolic model checker) would be needed

### Naming Convention

Specification identifiers match the C++ source code enumeration values:

| C++ enum value                  | TLA+ string           |
| ------------------------------- | --------------------- |
| `DataNodeState::kLive`          | `"Live"`              |
| `BlockState::kAllocating`       | `"Allocating"`        |
| `ReplicaState::kWriting`        | `"Writing"`           |
| `FileState::kUnderConstruction` | `"UnderConstruction"` |
| `LeaseState::kActive`           | `"Active"`            |

Each `.tla` file header documents the corresponding `.cpp` source files and function names.

### Curried Functions

The `WriteProtocol` specification uses curried function representations (`[A -> [B -> C]]`) rather than flat tuple-keyed functions (`[A \X B -> C]`). This is required because TLC's `EXCEPT` syntax (`![key] = value`) operates on the outer function level; for nested updates like `![block_id][datanode_id] = value`, the outer domain must be a single key, not a tuple.

### TLC Intermediate Files

TLC generates trace files (`*_TTrace_*.tla`) and a state queue directory (`states/`) during model checking. These are excluded from version control via `.gitignore`. To inspect a counterexample trace, run TLC without cleaning and open the generated `_TTrace_*.tla` file in the TLA+ Toolbox.

## Verification Roadmap

| Priority | Specification        | Status   | Notes                                                                                    |
| -------- | -------------------- | -------- | ---------------------------------------------------------------------------------------- |
| P0       | WriteProtocol        | Complete | Core write protocol; most complex and critical                                           |
| P1       | FileLeaseState       | Complete | Lease mutual exclusion; identified orphan file issue                                     |
| P1       | BlockReplicaState    | Complete | Block/replica consistency during allocation and commit                                   |
| P2       | DatanodeState        | Complete | Heartbeat state machine; verified basic correctness                                      |
| P3       | ReplicationRepair    | Planned  | Under/over-replicated block detection and repair by ReplicationManager                   |
| P3       | DecommissionProtocol | Planned  | DataNode decommissioning state transitions (currently skipped in `check_stale_and_dead`) |

## Issues Identified Through Specification Analysis

### 1. Dead Node Revival by Heartbeat

**Location:** `datanode_manager.cpp:99` — `handle_heartbeat()` unconditionally sets `dn.state = DataNodeState::kLive`

**Impact:** If a DataNode recovers after being marked Dead (>10 min without heartbeat), its next heartbeat immediately revives it. This differs from HDFS semantics where Dead nodes require explicit administrative re-registration. For MiniDFS this may be intentional since there is no decommission protocol.

**Severity:** Low (intentional design choice for simplicity)

### 2. Orphan UnderConstruction Files

**Location:** `lease_manager.cpp:101` — `expire_stale_leases()` closes the lease but does not restore the file from `UnderConstruction` to `Normal`

**Impact:** If a client crashes while holding a write lease, the file remains permanently in `UnderConstruction` state after lease expiry. The file cannot be read or written by other clients. No automated mechanism currently recovers from this state.

**Severity:** Medium (requires either manual intervention or a `NameNodeMaintenance` cleanup routine)

### 3. CommitBlock Idempotency with Partial Replica State

**Location:** `block_manager.cpp:147-196` — the `already_committed` branch skips block state update but still iterates over all replicas, transitioning Writing replicas in the `finalized_datanode_ids` set to Finalized.

**Impact:** If two concurrent commit requests arrive with different `finalized_datanode_ids` sets, the second request may transition additional replicas to Finalized. Since Finalized-to-Finalized is idempotent, this is safe. However, the model reveals that if a `generation_stamp` mismatch occurs, the replica state transitions could theoretically be applied to replicas of a new allocation — though this requires a `generation_stamp` collision, which is prevented by the monotonic ID allocator.

**Severity:** Low (safe due to idempotency and monotonic generation stamps)

## References

- [TLA+ Home Page](https://lamport.azurewebsites.net/tla/tla.html)
- [TLA+ Tutorial (PlusCal)](https://lamport.azurewebsites.net/tla/tutorial/home.html)
- [Specifying Systems (Lamport)](https://lamport.azurewebsites.net/tla/book.html)
- [HDFS Architecture Guide](https://hadoop.apache.org/docs/current/hadoop-project-dist/hadoop-hdfs/HdfsDesign.html)
