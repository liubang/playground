------------------------------- MODULE FileLeaseState -------------------------------

\* MiniDFS File + Lease 状态机验证
\*
\* File 状态 (Inode):
\*   Normal <-> UnderConstruction (create_file / complete_file / begin_append)
\*   Normal/UnderConstruction -> Deleted (delete)
\*
\* Lease 状态:
\*   Active -> Closed (release / expire)
\*
\* 关键不变量:
\*   1. 一个文件最多同时持有一个 Active lease (互斥写)
\*   2. 只有 UnderConstruction 的文件才可能有 Active lease
\*   3. 文件被删除后不应该有 Active lease
\*
\* 对应代码:
\*   - cpp/pl/minidfs/namenode/namespace_manager.cpp (create_file, complete_file, delete)
\*   - cpp/pl/minidfs/namenode/lease_manager.cpp (acquire, renew, release, expire)
\*   - cpp/pl/minidfs/common/types.h (FileState, LeaseState)

EXTENDS Naturals, TLC

CONSTANTS
    Files,           \* 文件集合 (inode)
    Clients,         \* 客户端集合
    LeaseTimeoutMs,  \* lease 超时时间
    MaxTime          \* 时间上限 (约束状态空间)

(***************************************************************************)
(* 变量                                                                    *)
(***************************************************************************)

VARIABLES
    file_state,      \* [Files -> {"Normal","UnderConstruction","Deleted"}]
    lease_state,     \* [Files -> {"None","Active","Closed"}]
    lease_holder,    \* [Files -> Clients]  当前持有 lease 的 client (lease_state=Active 时有效)
    lease_expire,    \* [Files -> Nat]  lease 过期时间 (lease_state=Active 时有效)
    now              \* 当前时钟

vars == <<file_state, lease_state, lease_holder, lease_expire, now>>

(***************************************************************************)
(* 初始状态: 所有文件为 Normal，无 lease                                  *)
(***************************************************************************)

Init ==
    /\ file_state   = [f \in Files |-> "Normal"]
    /\ lease_state  = [f \in Files |-> "None"]
    /\ lease_holder = [f \in Files |-> "none"]
    /\ lease_expire = [f \in Files |-> 0]
    /\ now = 0

(***************************************************************************)
(* 动作                                                                    *)
(***************************************************************************)

\* 创建文件: Normal -> UnderConstruction，同时授予第一个 lease
CreateFile(f, client) ==
    /\ file_state[f] = "Normal"
    /\ lease_state[f] = "None"
    /\ file_state'  = [file_state  EXCEPT ![f] = "UnderConstruction"]
    /\ lease_state' = [lease_state EXCEPT ![f] = "Active"]
    /\ lease_holder' = [lease_holder EXCEPT ![f] = client]
    /\ lease_expire' = [lease_expire EXCEPT ![f] = now + LeaseTimeoutMs]
    /\ UNCHANGED now

\* 完成写文件: UnderConstruction -> Normal，关闭 lease
CompleteFile(f, client) ==
    /\ file_state[f] = "UnderConstruction"
    /\ lease_state[f] = "Active"
    /\ lease_holder[f] = client
    /\ file_state'  = [file_state  EXCEPT ![f] = "Normal"]
    /\ lease_state' = [lease_state EXCEPT ![f] = "Closed"]
    /\ UNCHANGED <<lease_holder, lease_expire, now>>

\* 追加写入: Normal -> UnderConstruction，获取新 lease
BeginAppend(f, client) ==
    /\ file_state[f] = "Normal"
    /\ lease_state[f] = "None"
    /\ file_state'  = [file_state  EXCEPT ![f] = "UnderConstruction"]
    /\ lease_state' = [lease_state EXCEPT ![f] = "Active"]
    /\ lease_holder' = [lease_holder EXCEPT ![f] = client]
    /\ lease_expire' = [lease_expire EXCEPT ![f] = now + LeaseTimeoutMs]
    /\ UNCHANGED now

\* 续租
RenewLease(f, client) ==
    /\ lease_state[f] = "Active"
    /\ lease_holder[f] = client
    /\ lease_expire' = [lease_expire EXCEPT ![f] = now + LeaseTimeoutMs]
    /\ UNCHANGED <<file_state, lease_state, lease_holder, now>>

\* 释放 lease (客户端主动释放)
ReleaseLease(f, client) ==
    /\ lease_state[f] = "Active"
    /\ lease_holder[f] = client
    /\ lease_state' = [lease_state EXCEPT ![f] = "Closed"]
    /\ UNCHANGED <<file_state, lease_holder, lease_expire, now>>

\* 删除文件
DeleteFile(f) ==
    /\ file_state[f] \in {"Normal", "UnderConstruction"}
    /\ file_state' = [file_state EXCEPT ![f] = "Deleted"]
    \* 如果文件正在写入，强制关闭 lease
    /\ lease_state' = [lease_state EXCEPT ![f] = "Closed"]
    /\ UNCHANGED <<lease_holder, lease_expire, now>>

\* 时间推进 + 自动过期所有到期 lease
AdvanceTime ==
    \E t \in (now + 1)..MaxTime :
        /\ now' = t
        /\ lease_state' = [f \in Files |->
                IF lease_state[f] = "Active" /\ t > lease_expire[f]
                THEN "Closed"
                ELSE lease_state[f]]
        /\ UNCHANGED <<file_state, lease_holder, lease_expire>>

(***************************************************************************)
(* 下一步关系                                                              *)
(***************************************************************************)

Next ==
    \/ AdvanceTime
    \/ \E f \in Files, c \in Clients : CreateFile(f, c)
    \/ \E f \in Files, c \in Clients : CompleteFile(f, c)
    \/ \E f \in Files, c \in Clients : BeginAppend(f, c)
    \/ \E f \in Files, c \in Clients : RenewLease(f, c)
    \/ \E f \in Files, c \in Clients : ReleaseLease(f, c)
    \/ \E f \in Files : DeleteFile(f)

Terminating == UNCHANGED vars

Spec == Init /\ [][Next \/ Terminating]_vars

(***************************************************************************)
(* 不变量                                                                  *)
(***************************************************************************)

\* 类型约束
TypeOK ==
    /\ \A f \in Files : file_state[f] \in {"Normal", "UnderConstruction", "Deleted"}
    /\ \A f \in Files : lease_state[f] \in {"None", "Active", "Closed"}

\* 核心不变量: 每个文件最多一个 Active lease (互斥)
\* 注意: 这里用 lease_state 直接建模 (None/Active/Closed)，天然保证互斥

\* 只有 UnderConstruction 的文件才可能有 Active lease
OnlyUCFileHasActiveLease ==
    \A f \in Files :
        lease_state[f] = "Active" => file_state[f] = "UnderConstruction"

\* 已删除文件不应该有 Active lease
DeletedFileHasNoActiveLease ==
    \A f \in Files :
        file_state[f] = "Deleted" => lease_state[f] /= "Active"

\* Active lease 必须有合法的过期时间 (>= now: 过期瞬间仍有效)
ActiveLeaseHasFutureExpiry ==
    \A f \in Files :
        lease_state[f] = "Active" =>
            lease_expire[f] >= now
            /\ lease_expire[f] <= now + LeaseTimeoutMs

\* 如果文件是 Normal 且没有 active lease，lease_state 必须是 None 或 Closed
NormalFileLeaseConsistency ==
    \A f \in Files :
        file_state[f] = "Normal" =>
            lease_state[f] \in {"None", "Closed"}

FileLeaseInvariants ==
    TypeOK
    /\ OnlyUCFileHasActiveLease
    /\ DeletedFileHasNoActiveLease
    /\ ActiveLeaseHasFutureExpiry
    /\ NormalFileLeaseConsistency

(***************************************************************************)
(* 时序属性                                                                *)
(***************************************************************************)

\* 活跃性: 过期的 lease 最终会被关闭
\* ExpiredLeaseEventuallyClosed == \A f \in Files :
\*     now >= lease_expire[f] /\ lease_state[f] = "Active" ~>
\*         lease_state[f] = "Closed"

=============================================================================
\* Modification History
\* Last modified Thu Jun 26 23:30:00 CST 2026 by liubang
\* Created Thu Jun 26 23:30:00 CST 2026 by liubang
