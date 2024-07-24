------------------------------ MODULE Transfer ------------------------------
EXTENDS Naturals, TLC

(* --algorithm transfer {

variables alice_account = 10, bob_account = 10,
          account_total = alice_account + bob_account;
          
          
process (Transfer \in 1..2)
    variable money \in 1..20;
{
Transfer:
    if (alice_account >= money) {
       alice_account := alice_account - money;
       bob_account := bob_account + money;
    };
C: assert alice_account >= 0;
}

} algorithm *)

\* BEGIN TRANSLATION (chksum(pcal) = "4621a15e" /\ chksum(tla) = "e032f23b")
\* Label Transfer of process Transfer at line 14 col 5 changed to Transfer_
VARIABLES alice_account, bob_account, account_total, pc, money

vars == << alice_account, bob_account, account_total, pc, money >>

ProcSet == (1..2)

Init == (* Global variables *)
        /\ alice_account = 10
        /\ bob_account = 10
        /\ account_total = alice_account + bob_account
        (* Process Transfer *)
        /\ money \in [1..2 -> 1..20]
        /\ pc = [self \in ProcSet |-> "Transfer_"]

Transfer_(self) == /\ pc[self] = "Transfer_"
                   /\ IF alice_account >= money[self]
                         THEN /\ alice_account' = alice_account - money[self]
                              /\ bob_account' = bob_account + money[self]
                         ELSE /\ TRUE
                              /\ UNCHANGED << alice_account, bob_account >>
                   /\ pc' = [pc EXCEPT ![self] = "C"]
                   /\ UNCHANGED << account_total, money >>

C(self) == /\ pc[self] = "C"
           /\ Assert(alice_account >= 0, 
                     "Failure of assertion at line 18, column 4.")
           /\ pc' = [pc EXCEPT ![self] = "Done"]
           /\ UNCHANGED << alice_account, bob_account, account_total, money >>

Transfer(self) == Transfer_(self) \/ C(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in 1..2: Transfer(self))
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION 

MoneyNotNegative == money >= 0
MoneyInvariant == alice_account + bob_account = account_total

=============================================================================
\* Modification History
\* Last modified Wed Jul 24 17:24:41 CST 2024 by liubang01
\* Created Wed Jul 24 15:08:38 CST 2024 by liubang01
