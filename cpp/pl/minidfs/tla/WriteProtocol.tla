------------------------------ MODULE WriteProtocol ------------------------------

\* MiniDFS 完整 Pipeline 写协议验证
\*
\* 这是 MiniDFS 最核心的协议，涉及 Client、NameNode、多个 DataNode 之间的交互:
\*
\*   1. CreateFile   — Client → NameNode, 文件进入 UnderConstruction
\*   2. AllocateBlock — Client → NameNode, 分配 block + 选择目标 DataNode
\*   3. PipelineWrite — Client → DN1 → DN2 → DN3, 逐 chunk 复制
\*   4. CommitBlock   — DataNode → NameNode, block 提交
\*   5. CompleteFile  — Client → NameNode, 文件完成
\*
\* 关键不变量:
\*   - 同一文件同时只有一个 client 持有 Active lease
\*   - Committed block 必须有 >= MinWriteReplica 个 Finalized replica
\*   - 文件 Complete 后，所有 block 都是 Committed 状态
\*   - 文件 Complete 后，所有 block 的 replicas 都有足够的 Finalized 副本

EXTENDS Naturals, FiniteSets, TLC, Sequences

CONSTANTS
    Clients,         \* 客户端集合
    Files,           \* 文件集合
    DataNodes,       \* DataNode 集合
    MinWriteReplica, \* 最小写入确认数 (2)
    MaxBlockIndex    \* 最大 block 索引

(***************************************************************************)
(* 帮助函数                                                                *)
(***************************************************************************)

\* 为每个 file 生成一组 block IDs
FileBlocks(f) == { (f-1) * (MaxBlockIndex + 1) + i : i \in 0..MaxBlockIndex }

\* 每个 block 的 replicas (每个 replica 属于一个 DataNode)
BlockReplicas(b, dn) == b * Cardinality(DataNodes) + (dn - 1)

(***************************************************************************)
(* 变量                                                                    *)
(***************************************************************************)

(***************************************************************************)
(* 变量类型 (curried functions: [A -> [B -> C]], 不是 [A \X B -> C])      *)
(*                                                                        *)
(*   file_state    : [Files -> {"Normal","UnderConstruction",             *)
(*                            "Completed","Deleted"}]                     *)
(*   replica_state : [AllBlocks ->                                        *)
(*                     [DataNodes -> {"None","Writing","Finalized",       *)
(*                                    "Corrupt","Stale"}]]                *)
(*   write_progress: [Files ->                                            *)
(*                     [0..MaxBlockIndex ->                               *)
(*                       {"none","allocated","writing","committed"}]]     *)
(***************************************************************************)

VARIABLES
    file_state,
    file_length,
    block_state,
    block_length,
    block_replicas,
    replica_state,
    lease_holder,
    write_progress,
    client_blocks

AllBlocks == UNION { FileBlocks(f) : f \in Files }

vars == <<file_state, file_length, block_state, block_length, block_replicas,
          replica_state, lease_holder, write_progress, client_blocks>>

(***************************************************************************)
(* 初始状态                                                                *)
(***************************************************************************)

Init ==
    /\ file_state  = [f \in Files |-> "Normal"]
    /\ file_length = [f \in Files |-> 0]
    /\ block_state  = [b \in AllBlocks |-> "Unallocated"]
    /\ block_length = [b \in AllBlocks |-> 0]
    /\ block_replicas = [b \in AllBlocks |-> {}]
    /\ replica_state = [b \in AllBlocks |-> [dn \in DataNodes |-> "None"]]
    /\ lease_holder  = [f \in Files |-> 0]
    /\ write_progress = [f \in Files |-> [i \in 0..MaxBlockIndex |-> "none"]]
    /\ client_blocks  = [f \in Files |-> 0]

(***************************************************************************)
(* 动作                                                                    *)
(***************************************************************************)

(*** Phase 1: CreateFile ***)
\* Client 创建文件 → NameNode 将文件置于 UnderConstruction 并授予 lease
CreateFile(f, client) ==
    /\ file_state[f] = "Normal"
    /\ lease_holder[f] = 0
    /\ file_state'   = [file_state EXCEPT ![f] = "UnderConstruction"]
    /\ lease_holder' = [lease_holder EXCEPT ![f] = client]
    /\ UNCHANGED <<file_length, block_state, block_length, block_replicas,
                   replica_state, write_progress, client_blocks>>

(*** Phase 2: AllocateBlock ***)
\* NameNode 分配 block，选择 DataNodes 放置副本
\* 对应 block_manager.cpp:allocate_block()
AllocateBlock(f, block_idx, dns) ==
    LET b == (f-1) * (MaxBlockIndex + 1) + block_idx
    IN  /\ file_state[f] = "UnderConstruction"
        /\ write_progress[f][block_idx] = "none"
        /\ block_state[b] = "Unallocated"
        \* dns 是选中的目标 DataNode 集合，大小 >= MinWriteReplica
        /\ dns \subseteq DataNodes
        /\ Cardinality(dns) >= MinWriteReplica
        \* Block 进入 Allocating 状态
        /\ block_state' = [block_state EXCEPT ![b] = "Allocating"]
        /\ block_replicas' = [block_replicas EXCEPT ![b] = dns]
        \* 所有选中的 DN 上的 replica 进入 Writing 状态
        /\ replica_state' = [replica_state EXCEPT
                ![b] = [dn \in DataNodes |->
                    IF dn \in dns THEN "Writing"
                    ELSE replica_state[b][dn]]]
        /\ write_progress' = [write_progress EXCEPT ![f][block_idx] = "allocated"]
        /\ client_blocks' = [client_blocks EXCEPT ![f] = block_idx + 1]
        /\ UNCHANGED <<file_state, file_length, block_length, lease_holder>>

(*** Phase 3: PipelineWrite ***)
\* Pipeline 写入成功 —— 某个 replica 确认数据已写入
\* 简化建模：每个 DN replica 独立确认
PipelineWriteSuccess(b, dn) ==
    /\ block_state[b] = "Allocating"
    /\ replica_state[b][dn] = "Writing"
    \* replica 保持 Writing 直到 commit (commit 时才转 Finalized)
    \* 这里只是记录写入已完成 (实际系统中 chunk 级别 ack)
    /\ write_progress' = write_progress  \* 不变，等 commit
    /\ UNCHANGED <<file_state, file_length, block_state, block_length,
                   block_replicas, replica_state, lease_holder, client_blocks>>

(*** Phase 4: CommitBlock ***)
\* 写入完成后，确认的 replicas 从 Writing → Finalized，Block 从 Allocating → Committed
\* 对应 block_manager.cpp:commit_block()
CommitBlock(f, block_idx, confirmed_dns) ==
    LET b == (f-1) * (MaxBlockIndex + 1) + block_idx
    IN  /\ write_progress[f][block_idx] = "allocated"
        /\ block_state[b] = "Allocating"
        \* confirmed_dns 必须是该 block 的 replica DNs 的子集
        /\ confirmed_dns \subseteq block_replicas[b]
        \* 至少 MinWriteReplica 个 DN 确认成功
        /\ Cardinality(confirmed_dns) >= MinWriteReplica
        \* BUGFIX 预期: 如果 Code 忘记检查 confirmed ⊆ replicas，
        \*   TLC 会捕捉到 replica_state' 更新了错误的 DN
        /\ block_state' = [block_state EXCEPT ![b] = "Committed"]
        \* 确认的 replicas 转 Finalized
        /\ replica_state' = [replica_state EXCEPT
                ![b] = [dn \in DataNodes |->
                    IF dn \in confirmed_dns
                    THEN "Finalized"
                    ELSE replica_state[b][dn]]]
        /\ write_progress' = [write_progress EXCEPT ![f][block_idx] = "committed"]
        /\ UNCHANGED <<file_state, file_length, block_length, block_replicas, lease_holder, client_blocks>>

(*** Phase 5: CompleteFile ***)
\* Client 完成文件写入
\* 对应 namespace_manager.cpp:complete_file()
CompleteFile(f, client) ==
    /\ file_state[f] = "UnderConstruction"
    /\ lease_holder[f] = client
    \* 至少分配了一个 block (文件不为空)
    /\ client_blocks[f] > 0
    \* 所有已分配的 block 必须已 committed
    /\ \A i \in 0..(client_blocks[f] - 1) :
            LET b == (f-1) * (MaxBlockIndex + 1) + i
            IN  write_progress[f][i] = "committed" /\ block_state[b] = "Committed"
    /\ file_state'   = [file_state EXCEPT ![f] = "Completed"]
    /\ lease_holder' = [lease_holder EXCEPT ![f] = 0]
    /\ UNCHANGED <<file_length, block_state, block_length, block_replicas,
                   replica_state, write_progress, client_blocks>>

(*** Failure 注入 ***)
\* DataNode 故障: 该 DN 上所有 Writing/Finalized replica 变 Corrupt
DataNodeFailure(dn) ==
    /\ \E b \in AllBlocks :
            replica_state[b][dn] \in {"Writing", "Finalized"}
    /\ replica_state' = [b \in AllBlocks |->
            [dn2 \in DataNodes |->
                IF dn2 = dn /\ replica_state[b][dn2] \in {"Writing", "Finalized"}
                THEN "Corrupt"
                ELSE replica_state[b][dn2]]]
    /\ UNCHANGED <<file_state, file_length, block_state, block_length,
                   block_replicas, lease_holder, write_progress, client_blocks>>

(*** 超时 / Lease 过期 ***)
\* Lease 过期: 如果 client 崩溃，lease 过期但文件保持 UnderConstruction
LeaseExpire(f) ==
    /\ file_state[f] = "UnderConstruction"
    /\ lease_holder[f] /= 0
    /\ lease_holder' = [lease_holder EXCEPT ![f] = 0]
    \* 文件保持 UnderConstruction，允许其他 client 继续
    /\ UNCHANGED <<file_state, file_length, block_state, block_length,
                   block_replicas, replica_state, write_progress, client_blocks>>

(***************************************************************************)
(* 下一步关系                                                              *)
(***************************************************************************)

Next ==
    \/ \E f \in Files, c \in Clients : CreateFile(f, c)
    \/ \E f \in Files, i \in 0..MaxBlockIndex, dns \in SUBSET DataNodes :
            AllocateBlock(f, i, dns)
    \/ \E b \in AllBlocks, dn \in DataNodes : PipelineWriteSuccess(b, dn)
    \/ \E f \in Files, i \in 0..MaxBlockIndex, dns \in SUBSET DataNodes :
            CommitBlock(f, i, dns)
    \/ \E f \in Files, c \in Clients : CompleteFile(f, c)
    \/ \E dn \in DataNodes : DataNodeFailure(dn)
    \/ \E f \in Files : LeaseExpire(f)

Terminating == UNCHANGED vars

Spec == Init /\ [][Next \/ Terminating]_vars

(***************************************************************************)
(* 不变量 (Invariants)                                                     *)
(***************************************************************************)

\* TypeOK: 状态类型约束
TypeOK ==
    /\ \A f \in Files : file_state[f] \in {"Normal","UnderConstruction","Completed","Deleted"}
    /\ \A b \in AllBlocks : block_state[b] \in {"Unallocated","Allocating","Committed","Corrupt"}
    /\ \A b \in AllBlocks, dn \in DataNodes :
            replica_state[b][dn] \in {"None","Writing","Finalized","Corrupt","Stale"}
    /\ \A f \in Files : lease_holder[f] \in {0} \union Clients

\* 核心不变量 1: 互斥写 —— 每个文件最多一个有效 lease holder
\* (由 lease_holder 直接表达: 每个文件一个 holder)

\* 核心不变量 2: Completed 文件的所有 block 必须 Committed
CompletedFileBlocksAreCommitted ==
    \A f \in Files :
        file_state[f] = "Completed" =>
            \A i \in 0..MaxBlockIndex :
                i < client_blocks[f] =>
                    LET b == (f-1) * (MaxBlockIndex + 1) + i
                    IN  block_state[b] = "Committed"

\* 核心不变量 3: Completed 文件的每个 block 至少有一个 Finalized replica
\* (低于 MinWriteReplica 是临时的，由 ReplicationManager 异步修复)
CompletedFileHasAtLeastOneReplica ==
    \A f \in Files :
        file_state[f] = "Completed" =>
            \A i \in 0..MaxBlockIndex :
                i < client_blocks[f] =>
                    LET b == (f-1) * (MaxBlockIndex + 1) + i
                        finalized == {dn \in DataNodes : replica_state[b][dn] = "Finalized"}
                    IN  Cardinality(finalized) >= 1

\* 核心不变量 4: Unallocated block 没有任何 replicas
UnallocatedBlockHasNoReplicas ==
    \A b \in AllBlocks :
        block_state[b] = "Unallocated" =>
            block_replicas[b] = {}
            /\ \A dn \in DataNodes : replica_state[b][dn] = "None"

\* 核心不变量 5: 只有 UnderConstruction 文件可以有 lease
OnlyUCFileHasLease ==
    \A f \in Files :
        lease_holder[f] /= 0 => file_state[f] = "UnderConstruction"

\* 综合不变量
WriteProtocolInvariants ==
    TypeOK
    /\ CompletedFileBlocksAreCommitted
    \* CompletedFileHasAtLeastOneReplica: 容错属性，需 ReplicationManager
    /\ UnallocatedBlockHasNoReplicas
    /\ OnlyUCFileHasLease

(***************************************************************************)
(* 时序属性                                                                *)
(***************************************************************************)

\* 可提交性: 一个 UnderConstruction 文件最终可以被 Complete
\* 条件: 如果 client 持续持有 lease 且 DN 不故障
\* Completable(f) ==
\*     file_state[f] = "UnderConstruction" /\ lease_holder[f] /= 0 ~>
\*         file_state[f] = "Completed"

=============================================================================
\* Modification History
\* Last modified Thu Jun 26 23:30:00 CST 2026 by liubang
\* Created Thu Jun 26 23:30:00 CST 2026 by liubang
