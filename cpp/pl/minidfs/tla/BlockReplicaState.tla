------------------------------- MODULE BlockReplicaState -------------------------------

\* MiniDFS Block + Replica 状态机联合验证
\*
\* Block 状态:
\*   Allocating -> Committed (via commit_block)
\*   Allocating -> Corrupt   (heartbeat report)
\*   Committed  -> Corrupt   (heartbeat report)
\*   Allocating/Committed/Corrupt -> Deleted (file deletion)
\*
\* Replica 状态:
\*   Writing -> Finalized (block committed + this replica confirmed)
\*   Writing -> Corrupt   (DataNode reported corruption)
\*   Finalized -> Stale   (generation_stamp mismatch / DN re-register)
\*   Finalized -> Corrupt (DataNode reported corruption)
\*   Writing/Finalized/Corrupt/Stale -> Deleting -> Deleted
\*
\* 对应代码:
\*   - cpp/pl/minidfs/namenode/block_manager.cpp  (allocate_block, commit_block)
\*   - cpp/pl/minidfs/namenode/replication_manager.cpp (scan, generate tasks)
\*   - cpp/pl/minidfs/common/types.h (BlockState, ReplicaState 枚举)

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Blocks,         \* Block 集合
    Replicas,       \* Replica 集合 (每个 replica 属于一个 block，位于一个 DataNode)
    DataNodes,      \* DataNode 集合
    MinWriteReplica \* 最小写入副本数 (对应 kMinWriteReplica = 2)

(***************************************************************************)
(* 辅助关系                                                                *)
(***************************************************************************)

\* 每个 replica 属于一个 block (1-based)
ReplicaBlock(r) == ((r - 1) \div Cardinality(DataNodes)) + 1

\* 每个 replica 位于一个 DataNode (1-based)
ReplicaDataNode(r) == ((r - 1) % Cardinality(DataNodes)) + 1

(***************************************************************************)
(* 变量                                                                    *)
(***************************************************************************)

VARIABLES
    block_state,       \* [Blocks -> {"Allocating","Committed","Corrupt","Deleted"}]
    block_length,      \* [Blocks -> Nat]  块长度 (仅 committed 后有意义)
    replica_state,     \* [Replicas -> {"Writing","Finalized","Corrupt","Stale","Deleting","Deleted"}]
    replica_gen_stamp  \* [Replicas -> Nat]  generation stamp

vars == <<block_state, block_length, replica_state, replica_gen_stamp>>

(***************************************************************************)
(* 初始状态                                                                *)
(***************************************************************************)

Init ==
    /\ block_state      = [b \in Blocks |-> "Allocating"]
    /\ block_length     = [b \in Blocks |-> 0]
    /\ replica_state    = [r \in Replicas |-> "Writing"]
    /\ replica_gen_stamp = [r \in Replicas |-> 0]

(***************************************************************************)
(* 动作                                                                    *)
(***************************************************************************)

\* commit_block: 客户端完成 pipeline 写入后提交
\* 对应 block_manager.cpp:commit_block()
CommitBlock(b, replica_set) ==
    \* 前置条件: block 必须是 Allocating 状态
    /\ block_state[b] = "Allocating"
    \* replica_set 是提交中确认 completed 的 replica 子集，不能为空
    /\ replica_set \subseteq Replicas
    /\ replica_set /= {}
    \* 所有 replica 属于这个 block
    /\ \A r \in replica_set : ReplicaBlock(r) = b
    \* 至少 MinWriteReplica 个 replica 被确认
    /\ Cardinality({r \in replica_set : replica_state[r] = "Writing"}) >= MinWriteReplica
    \* 过渡: block -> Committed
    /\ block_state' = [block_state EXCEPT ![b] = "Committed"]
    \* replica_set 中的 replica 从 Writing -> Finalized
    /\ replica_state' = [r \in Replicas |->
            IF r \in replica_set /\ replica_state[r] = "Writing"
            THEN "Finalized"
            ELSE replica_state[r]]
    /\ UNCHANGED <<block_length, replica_gen_stamp>>

\* 副本变 Stale: generation_stamp 更新导致旧 replicas 变为 stale
\* 只允许 Committed block 的 replica 变 stale
\* (Allocating block 的 replica 只能通过 commit 或 corrupt 改变状态)
MarkReplicaStale(r) ==
    /\ replica_state[r] \in {"Finalized", "Writing"}
    /\ block_state[ReplicaBlock(r)] = "Committed"
    /\ replica_state' = [replica_state EXCEPT ![r] = "Stale"]
    /\ UNCHANGED <<block_state, block_length, replica_gen_stamp>>

\* 副本报告损坏 (DataNode 发现 checksum 错误)
MarkReplicaCorrupt(r) ==
    /\ replica_state[r] \in {"Writing", "Finalized", "Stale"}
    /\ replica_state' = [replica_state EXCEPT ![r] = "Corrupt"]
    /\ UNCHANGED <<block_state, block_length, replica_gen_stamp>>

\* Block 标记为损坏 (当没有足够的健康 replicas 时)
MarkBlockCorrupt(b) ==
    /\ block_state[b] \in {"Allocating", "Committed"}
    /\ block_state' = [block_state EXCEPT ![b] = "Corrupt"]
    /\ UNCHANGED <<block_length, replica_state, replica_gen_stamp>>

\* 删除流程: Replica -> Deleting -> Deleted
\* 只能删除已 Committed/Corrupt/Deleted block 的 replicas
StartDeleteReplica(r) ==
    /\ block_state[ReplicaBlock(r)] \in {"Committed", "Corrupt", "Deleted"}
    /\ replica_state[r] \in {"Writing", "Finalized", "Corrupt", "Stale"}
    /\ replica_state' = [replica_state EXCEPT ![r] = "Deleting"]
    /\ UNCHANGED <<block_state, block_length, replica_gen_stamp>>

FinishDeleteReplica(r) ==
    /\ replica_state[r] = "Deleting"
    /\ replica_state' = [replica_state EXCEPT ![r] = "Deleted"]
    /\ UNCHANGED <<block_state, block_length, replica_gen_stamp>>

\* 删除整个 Block (级联删除所有 replicas)
DeleteBlock(b) ==
    /\ block_state[b] \in {"Allocating", "Committed", "Corrupt"}
    /\ block_state' = [block_state EXCEPT ![b] = "Deleted"]
    \* 所有关联的 replicas 标记为 Deleting
    /\ replica_state' = [r \in Replicas |->
            IF ReplicaBlock(r) = b /\ replica_state[r] /= "Deleted"
            THEN "Deleting"
            ELSE replica_state[r]]
    /\ UNCHANGED <<block_length, replica_gen_stamp>>

(***************************************************************************)
(* 下一步关系                                                              *)
(***************************************************************************)

Next ==
    \/ \E b \in Blocks, rs \in SUBSET Replicas : CommitBlock(b, rs)
    \/ \E r \in Replicas : MarkReplicaStale(r)
    \/ \E r \in Replicas : MarkReplicaCorrupt(r)
    \/ \E b \in Blocks : MarkBlockCorrupt(b)
    \/ \E r \in Replicas : StartDeleteReplica(r)
    \/ \E r \in Replicas : FinishDeleteReplica(r)
    \/ \E b \in Blocks : DeleteBlock(b)

Terminating == UNCHANGED vars

Spec == Init /\ [][Next \/ Terminating]_vars

(***************************************************************************)
(* 不变量                                                                  *)
(***************************************************************************)

\* TypeOK: 状态在合法枚举范围内
TypeOK ==
    /\ \A b \in Blocks : block_state[b] \in {"Allocating","Committed","Corrupt","Deleted"}
    /\ \A r \in Replicas : replica_state[r] \in {"Writing","Finalized","Corrupt","Stale","Deleting","Deleted"}

\* Committed block 必须有足够 healthy replicas
\* (Stale replicas 数据仍在，可被 ReplicationManager 修复)
CommittedBlockHasMinReplicas ==
    \A b \in Blocks :
        block_state[b] = "Committed" =>
            Cardinality({r \in Replicas :
                    ReplicaBlock(r) = b
                    /\ replica_state[r] \in {"Writing", "Finalized", "Stale"}}) >= MinWriteReplica

\* Deleted block 不应该有任何非 Deleted 的 replica
DeletedBlockHasNoActiveReplicas ==
    \A b \in Blocks :
        block_state[b] = "Deleted" =>
            \A r \in Replicas :
                ReplicaBlock(r) = b =>
                    replica_state[r] \in {"Deleting", "Deleted"}

\* 一致性: Allocating block 的 replicas 只能是 Writing 或 Corrupt
\* (DataNode 可能在写入过程中报告损坏)
AllocatingBlockReplicasAreWritingOrCorrupt ==
    \A b \in Blocks :
        block_state[b] = "Allocating" =>
            \A r \in Replicas :
                ReplicaBlock(r) = b =>
                    replica_state[r] \in {"Writing", "Corrupt"}

BlockReplicaInvariants ==
    TypeOK
    /\ DeletedBlockHasNoActiveReplicas
    /\ AllocatingBlockReplicasAreWritingOrCorrupt
    \* CommittedBlockHasMinReplicas 是容错属性，需要 ReplicationManager 模型

=============================================================================
\* Modification History
\* Last modified Thu Jun 26 23:30:00 CST 2026 by liubang
\* Created Thu Jun 26 23:30:00 CST 2026 by liubang
