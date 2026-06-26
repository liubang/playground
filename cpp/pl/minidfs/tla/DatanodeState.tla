------------------------------- MODULE DatanodeState -------------------------------

\* MiniDFS DataNode 生命周期状态机验证
\*
\* 状态转换:
\*   (new) -> Live -> Stale -> Dead
\*   Live <-> Stale  (heartbeat 回归)
\*   Dead -> Live    (heartbeat / re-register 回归)
\*
\* 对应代码: cpp/pl/minidfs/namenode/datanode_manager.cpp
\*   - register_datanode() : new/Live -> Live
\*   - handle_heartbeat()  : any -> Live
\*   - check_stale_and_dead() : Live/Stale -> Stale/Dead based on elapsed time

EXTENDS Naturals, TLC

CONSTANTS
    \* DataNode 集合
    DataNodes,
    \* 超时阈值 (ms)
    StaleTimeoutMs,
    DeadTimeoutMs,
    \* 时间边界 (用于 TLC 模型检查时的有限状态空间)
    MaxTime

(***************************************************************************)
(* 纯 TLA+ 规格 —— 时间通过 nondeterministic 递增来建模                    *)
(***************************************************************************)

VARIABLES
    dn_state,         \* [DataNodes -> {"Live","Stale","Dead"}]
    dn_last_hb,       \* [DataNodes -> 0..MaxTime]  最后一次心跳时间
    now               \* 0..MaxTime  当前全局时钟

vars == <<dn_state, dn_last_hb, now>>

(***************************************************************************)
(* 初始状态                                                               *)
(***************************************************************************)

Init ==
    /\ dn_state  = [dn \in DataNodes |-> "Live"]
    /\ dn_last_hb = [dn \in DataNodes |-> 0]
    /\ now = 0

(***************************************************************************)
(* 帮助函数                                                                *)
(***************************************************************************)

Elapsed(dn) == now - dn_last_hb[dn]

\* StateByElapsed 使用当前 now；StateAtTime 使用给定时间 t
StateAtTime(dn, t) ==
    IF t - dn_last_hb[dn] >= DeadTimeoutMs  THEN "Dead"
    ELSE IF t - dn_last_hb[dn] >= StaleTimeoutMs THEN "Stale"
    ELSE "Live"

StateByElapsed(dn) == StateAtTime(dn, now)

(***************************************************************************)
(* 动作                                                                    *)
(***************************************************************************)

\* 时间推进 + 状态扫描 (合并 AdvanceTime + CheckStaleAndDead)
\* 对应 check_stale_and_dead() 在 NameNodeMaintenance 的周期性回调中执行
TickAndScan ==
    \E t \in (now + 1)..MaxTime :
        /\ now' = t
        /\ dn_state' = [dn \in DataNodes |-> StateAtTime(dn, t)]
        /\ UNCHANGED dn_last_hb

\* DataNode 发送心跳，NameNode 接收后强制设为 Live
Heartbeat(dn) ==
    /\ dn_last_hb' = [dn_last_hb EXCEPT ![dn] = now]
    /\ dn_state'  = [dn_state  EXCEPT ![dn] = "Live"]
    /\ UNCHANGED now

(***************************************************************************)
(* 下一步关系                                                              *)
(***************************************************************************)

Next ==
    \/ TickAndScan
    \/ \E dn \in DataNodes : Heartbeat(dn)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* 不变量 (Invariants)                                                     *)
(***************************************************************************)

\* TypeOK: 状态只能是三种合法值之一
TypeOK ==
    /\ \A dn \in DataNodes : dn_state[dn] \in {"Live", "Stale", "Dead"}
    /\ \A dn \in DataNodes : dn_last_hb[dn] \in 0..now
    /\ now \in 0..MaxTime

\* 状态-时间一致性: 如果节点是 Live，其心跳距今必须在 stale 阈值内
LiveImpliesFreshHeartbeat ==
    \A dn \in DataNodes :
        dn_state[dn] = "Live" => Elapsed(dn) < StaleTimeoutMs

\* 状态-时间一致性: 如果节点是 Stale，其心跳距今必须在 stale 和 dead 之间
StaleImpliesAgedHeartbeat ==
    \A dn \in DataNodes :
        dn_state[dn] = "Stale" =>
            StaleTimeoutMs <= Elapsed(dn) /\ Elapsed(dn) < DeadTimeoutMs

\* 状态-时间一致性: 如果节点是 Dead，其心跳距今必须超过 dead 阈值
DeadImpliesExpiredHeartbeat ==
    \A dn \in DataNodes :
        dn_state[dn] = "Dead" => Elapsed(dn) >= DeadTimeoutMs

\* 综合: check_stale_and_dead 产生的结果与 StateByElapsed 一致
StateConsistency ==
    LiveImpliesFreshHeartbeat
    /\ StaleImpliesAgedHeartbeat
    /\ DeadImpliesExpiredHeartbeat

\* 检查逻辑本身的正确性 —— 如果没有 AdvanceTime，只通过 heartbeat 和 scan 演化，
\* 状态最终会稳定（所有节点要么 Live 要么 Dead，没有"卡在中间"的死锁）
\* 这不是一个不变量，而是一个稳定性条件。

(***************************************************************************)
(* 时序属性 (Temporal Properties)                                          *)
(***************************************************************************)

\* 活跃性: 如果某个节点持续收到心跳，它最终会是 Live
\* (注意: TLC 在有限状态模型中可以验证此类属性)
HeartbeatLeadsToLive(dn) ==
    [](dn_state[dn] = "Dead" =>
        <>(dn_state[dn] = "Live"))

=============================================================================
\* Modification History
\* Last modified Thu Jun 26 23:30:00 CST 2026 by liubang
\* Created Thu Jun 26 23:30:00 CST 2026 by liubang
