------------------------------- MODULE Demo1 -------------------------------
EXTENDS Integers, TLC

People == {"alice", "bob"} \* 定义一个集合
Money == 1..10             \* 定义集合
NumTransfers == 2

(*--algorithm wire
{
    variables acct \in [People -> Money]; \* [People -> Money] 是一个Set，代表账户所有可能情况
    
    define
    {
        NoOverdrafts == \A p \in People: acct[p] >= 0 \* 不变量约束
    }
    
    process(wire \in 1..NumTransfers) \* 模拟1~NumTransfers次转账
        variables
            amnt \in 1..5;
            from \in People;
            to \in People;
    {
        Check:
        {
            if (acct[from] >= amnt) 
            {
                Withdraw:
                {
                    acct[from] := acct[from] - amnt;
                };
                Desposit:
                {
                    acct[to] := acct[to] + amnt;
                }
            }
        }
    };
}
algorithm*)
\* BEGIN TRANSLATION (chksum(pcal) = "48b19a81" /\ chksum(tla) = "58d7d06a")
VARIABLES acct, pc

(* define statement *)
NoOverdrafts == \A p \in People: acct[p] >= 0

VARIABLES amnt, from, to

vars == << acct, pc, amnt, from, to >>

ProcSet == (1..NumTransfers)

Init == (* Global variables *)
        /\ acct \in [People -> Money]
        (* Process wire *)
        /\ amnt \in [1..NumTransfers -> 1..5]
        /\ from \in [1..NumTransfers -> People]
        /\ to \in [1..NumTransfers -> People]
        /\ pc = [self \in ProcSet |-> "Check"]

Check(self) == /\ pc[self] = "Check"
               /\ IF acct[from[self]] >= amnt[self]
                     THEN /\ pc' = [pc EXCEPT ![self] = "Withdraw"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
               /\ UNCHANGED << acct, amnt, from, to >>

Withdraw(self) == /\ pc[self] = "Withdraw"
                  /\ acct' = [acct EXCEPT ![from[self]] = acct[from[self]] - amnt[self]]
                  /\ pc' = [pc EXCEPT ![self] = "Desposit"]
                  /\ UNCHANGED << amnt, from, to >>

Desposit(self) == /\ pc[self] = "Desposit"
                  /\ acct' = [acct EXCEPT ![to[self]] = acct[to[self]] + amnt[self]]
                  /\ pc' = [pc EXCEPT ![self] = "Done"]
                  /\ UNCHANGED << amnt, from, to >>

wire(self) == Check(self) \/ Withdraw(self) \/ Desposit(self)

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
\* Last modified Tue Jul 23 17:40:28 CST 2024 by liubang01
\* Created Tue Jul 23 17:29:55 CST 2024 by liubang01
