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

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/22 21:10

package domain

import (
	"testing"
)

// TestLegalTransitions verifies every legal phase transition per §25.1.
func TestLegalTransitions(t *testing.T) {
	tests := []struct {
		from Phase
		to   Phase
	}{
		{PhasePreparing, PhaseCallingModel},
		{PhaseCallingModel, PhaseAwaitingApproval},
		{PhaseCallingModel, PhaseExecutingTools},
		{PhaseCallingModel, PhasePreparing},
		{PhaseAwaitingApproval, PhaseExecutingTools},
		{PhaseAwaitingApproval, PhasePreparing},
		{PhaseExecutingTools, PhaseCompacting},
		{PhaseExecutingTools, PhasePreparing},
		{PhaseCompacting, PhasePreparing},
	}

	for _, tt := range tests {
		if !CanTransition(tt.from, tt.to) {
			t.Errorf("CanTransition(%s, %s) = false, want true", tt.from, tt.to)
		}
	}
}

// TestIllegalTransitions checks some clearly illegal transitions.
func TestIllegalTransitions(t *testing.T) {
	illegal := []struct{ from, to Phase }{
		{PhasePreparing, PhaseExecutingTools},
		{PhasePreparing, PhaseCompacting},
		{PhaseAwaitingApproval, PhaseCallingModel},
		{PhaseCompacting, PhaseExecutingTools},
		{PhaseCompacting, PhaseCallingModel},
		{PhaseExecutingTools, PhaseAwaitingApproval},
	}

	for _, tt := range illegal {
		if CanTransition(tt.from, tt.to) {
			t.Errorf("CanTransition(%s, %s) = true, want false", tt.from, tt.to)
		}
	}
}

// TestSelfTransition is never allowed.
func TestSelfTransition(t *testing.T) {
	for _, p := range []Phase{PhasePreparing, PhaseCallingModel, PhaseAwaitingApproval, PhaseExecutingTools, PhaseCompacting} {
		if CanTransition(p, p) {
			t.Errorf("CanTransition(%s, %s) = true, want false", p, p)
		}
	}
}

// TestCanTerminate any phase can be cancelled.
func TestCanTerminate(t *testing.T) {
	for _, p := range []Phase{PhasePreparing, PhaseCallingModel, PhaseAwaitingApproval, PhaseExecutingTools, PhaseCompacting} {
		if !CanTerminate(p) {
			t.Errorf("CanTerminate(%s) = false, want true", p)
		}
	}
}

// TestRunStateTransition applies a legal transition.
func TestRunStateTransition(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing}
	newS, err := s.Transition(PhaseCallingModel)
	if err != nil {
		t.Fatalf("Transition error: %v", err)
	}
	if newS.Phase != PhaseCallingModel {
		t.Fatalf("expected phase %s, got %s", PhaseCallingModel, newS.Phase)
	}
	if newS.Lifecycle != LifecycleActive {
		t.Fatalf("expected lifecycle active, got %s", newS.Lifecycle)
	}
}

// TestRunStateTransitionIllegal rejects illegal transition.
func TestRunStateTransitionIllegal(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing}
	_, err := s.Transition(PhaseExecutingTools)
	if err == nil {
		t.Fatal("expected error for illegal transition")
	}
}

// TestRunStateTerminate sets terminal outcome.
func TestRunStateTerminate(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhaseCallingModel}
	ts, err := s.Terminate(OutcomeSucceeded)
	if err != nil {
		t.Fatalf("Terminate error: %v", err)
	}
	if ts.Lifecycle != LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", ts.Lifecycle)
	}
	if ts.Outcome != OutcomeSucceeded {
		t.Fatalf("expected succeeded, got %s", ts.Outcome)
	}
}

// TestRunStateTerminateAlreadyTerminal prevents double termination.
func TestRunStateTerminateAlreadyTerminal(t *testing.T) {
	s := RunState{Lifecycle: LifecycleTerminal, Outcome: OutcomeFailed}
	_, err := s.Terminate(OutcomeSucceeded)
	if err == nil {
		t.Fatal("expected error for terminating already-terminal run")
	}
}

// TestRunStateSuspend adds suspension reason.
func TestRunStateSuspend(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhaseAwaitingApproval}
	ss, err := s.Suspend(SuspensionApproval)
	if err != nil {
		t.Fatalf("Suspend error: %v", err)
	}
	if ss.Lifecycle != LifecycleSuspended {
		t.Fatalf("expected suspended, got %s", ss.Lifecycle)
	}
	if ss.SuspensionReason != SuspensionApproval {
		t.Fatalf("expected approval reason, got %s", ss.SuspensionReason)
	}
}

// TestRunStateSuspendRequiresReason.
func TestRunStateSuspendRequiresReason(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing}
	_, err := s.Suspend("")
	if err == nil {
		t.Fatal("expected error for suspend without reason")
	}
}

// TestRunStateSuspendTerminalFails.
func TestRunStateSuspendTerminalFails(t *testing.T) {
	s := RunState{Lifecycle: LifecycleTerminal, Outcome: OutcomeFailed}
	_, err := s.Suspend(SuspensionBudget)
	if err == nil {
		t.Fatal("expected error for suspending terminal run")
	}
}

// TestRunStateResume returns to active.
func TestRunStateResume(t *testing.T) {
	s := RunState{Lifecycle: LifecycleSuspended, Phase: PhaseAwaitingApproval, SuspensionReason: SuspensionApproval}
	rs, err := s.Resume()
	if err != nil {
		t.Fatalf("Resume error: %v", err)
	}
	if rs.Lifecycle != LifecycleActive {
		t.Fatalf("expected active, got %s", rs.Lifecycle)
	}
	if rs.Phase != PhaseAwaitingApproval {
		t.Fatalf("expected phase preserved, got %s", rs.Phase)
	}
}

// TestRunStateResumeOnlySuspended.
func TestRunStateResumeOnlySuspended(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing}
	_, err := s.Resume()
	if err == nil {
		t.Fatal("expected error for resuming non-suspended run")
	}
}

// TestRunStateValidate checks all invariants.
func TestRunStateValidate(t *testing.T) {
	tests := []struct {
		name    string
		state   RunState
		wantErr bool
	}{
		{
			"valid active",
			RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing},
			false,
		},
		{
			"active with outcome",
			RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing, Outcome: OutcomeSucceeded},
			true,
		},
		{
			"active with suspension reason",
			RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing, SuspensionReason: SuspensionApproval},
			true,
		},
		{
			"valid suspended",
			RunState{Lifecycle: LifecycleSuspended, Phase: PhaseAwaitingApproval, SuspensionReason: SuspensionApproval},
			false,
		},
		{
			"suspended without reason",
			RunState{Lifecycle: LifecycleSuspended, Phase: PhaseAwaitingApproval},
			true,
		},
		{
			"suspended with outcome",
			RunState{Lifecycle: LifecycleSuspended, Phase: PhaseAwaitingApproval, SuspensionReason: SuspensionApproval, Outcome: OutcomeFailed},
			true,
		},
		{
			"valid terminal",
			RunState{Lifecycle: LifecycleTerminal, Outcome: OutcomeSucceeded},
			false,
		},
		{
			"terminal without outcome",
			RunState{Lifecycle: LifecycleTerminal},
			true,
		},
		{
			"terminal with suspension reason",
			RunState{Lifecycle: LifecycleTerminal, Outcome: OutcomeFailed, SuspensionReason: SuspensionBudget},
			true,
		},
		{
			"invalid lifecycle",
			RunState{Lifecycle: "unknown"},
			true,
		},
		{
			"invalid phase",
			RunState{Lifecycle: LifecycleActive, Phase: "unknown"},
			true,
		},
		{
			"invalid outcome",
			RunState{Lifecycle: LifecycleTerminal, Outcome: "unknown"},
			true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.state.Validate()
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}

// TestFullRunLifecycle tests the complete state machine flow:
// created → preparing → calling_model → executing_tools → preparing → ... → completed
func TestFullRunLifecycle(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhasePreparing}

	// preparing -> calling_model
	s, _ = s.Transition(PhaseCallingModel)

	// calling_model -> executing_tools (no approval needed)
	s, _ = s.Transition(PhaseExecutingTools)

	// executing_tools -> preparing (next turn)
	s, _ = s.Transition(PhasePreparing)

	// preparing -> calling_model (second turn)
	s, _ = s.Transition(PhaseCallingModel)

	// calling_model -> terminal (model returns without tools)
	s, _ = s.Terminate(OutcomeSucceeded)

	if s.Lifecycle != LifecycleTerminal {
		t.Fatalf("expected terminal, got %s", s.Lifecycle)
	}
	if s.Outcome != OutcomeSucceeded {
		t.Fatalf("expected succeeded, got %s", s.Outcome)
	}
}

// TestRunLifecycleWithSuspendAndResume.
func TestRunLifecycleWithSuspendAndResume(t *testing.T) {
	s := RunState{Lifecycle: LifecycleActive, Phase: PhaseAwaitingApproval}

	// Suspend for approval
	s, _ = s.Suspend(SuspensionApproval)
	if s.Lifecycle != LifecycleSuspended {
		t.Fatalf("expected suspended, got %s", s.Lifecycle)
	}

	// Resume back
	s, _ = s.Resume()
	if s.Lifecycle != LifecycleActive {
		t.Fatalf("expected active, got %s", s.Lifecycle)
	}

	// Continue
	s, _ = s.Transition(PhaseExecutingTools)
	s, _ = s.Terminate(OutcomeSucceeded)
}
