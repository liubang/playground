------------------------------- MODULE Demo1 -------------------------------
EXTENDS Integers, TLC

People == {"alice", "bob"} \* 定义一个集合
Money == 1..10             \* 定义集合
NumTransfers == 2

(* --algorithm wire {

variables acct \in [People -> Money]; \* [People -> Money] 是一个Set，代表账户所有可能情况

define {
    NoOverdrafts == \A p \in People: acct[p] >= 0 \* 不变量约束
}

process(wire \in 1..NumTransfers) \* 模拟1~NumTransfers次转账
    variables
        amnt \in 1..6;
        from \in People;
        to \in People;
{
Check: 
    if (acct[from] >= amnt) {
        acct[from] := acct[from] - amnt ||
        acct[to] := acct[to] + amnt;
    };
}

} end algorithm *)

\* BEGIN TRANSLATION (chksum(pcal) = "aa01fc4f" /\ chksum(tla) = "ef804eb5")
VARIABLES acct, pc

(* define statement *)
NoOverdrafts == \A p \in People: acct[p] >= 0

VARIABLES amnt, from, to

vars == << acct, pc, amnt, from, to >>

ProcSet == (1..NumTransfers)

Init == (* Global variables *)
        /\ acct \in [People -> Money]
        (* Process wire *)
        /\ amnt \in [1..NumTransfers -> 1..6]
        /\ from \in [1..NumTransfers -> People]
        /\ to \in [1..NumTransfers -> People]
        /\ pc = [self \in ProcSet |-> "Check"]

Check(self) == /\ pc[self] = "Check"
               /\ IF acct[from[self]] >= amnt[self]
                     THEN /\ acct' = [acct EXCEPT ![from[self]] = acct[from[self]] - amnt[self],
                                                  ![to[self]] = acct[to[self]] + amnt[self]]
                     ELSE /\ TRUE
                          /\ acct' = acct
               /\ pc' = [pc EXCEPT ![self] = "Done"]
               /\ UNCHANGED << amnt, from, to >>

wire(self) == Check(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in 1..NumTransfers: wire(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 

=============================================================================
\* Modification History
\* Last modified Wed Jul 24 15:00:08 CST 2024 by liubang01
\* Created Tue Jul 23 17:29:55 CST 2024 by liubang01
