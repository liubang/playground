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

import "fmt"

// Lifecycle represents the top-level life-cycle state of a Run.
type Lifecycle string

const (
	LifecycleActive    Lifecycle = "active"
	LifecycleSuspended Lifecycle = "suspended"
	LifecycleTerminal  Lifecycle = "terminal"
)

// Phase represents the current execution phase of an active Run.
type Phase string

const (
	PhasePreparing        Phase = "preparing"
	PhaseCallingModel     Phase = "calling_model"
	PhaseAwaitingApproval Phase = "awaiting_approval"
	PhaseExecutingTools   Phase = "executing_tools"
	PhaseCompacting       Phase = "compacting"
)

// Outcome represents the final result of a terminal Run.
type Outcome string

const (
	OutcomeSucceeded           Outcome = "succeeded"
	OutcomeCompletedUnverified Outcome = "completed_unverified"
	OutcomeNeedsUser           Outcome = "needs_user"
	OutcomeBudgetExhausted     Outcome = "budget_exhausted"
	OutcomeFailed              Outcome = "failed"
	OutcomeCancelled           Outcome = "cancelled"
)

// SuspensionReason represents why a Run is suspended.
type SuspensionReason string

const (
	SuspensionApproval          SuspensionReason = "approval"
	SuspensionClarification     SuspensionReason = "clarification"
	SuspensionCredential        SuspensionReason = "credential"
	SuspensionExternalCondition SuspensionReason = "external_condition"
	SuspensionBudget            SuspensionReason = "budget"
	SuspensionOutcomeUnknown    SuspensionReason = "outcome_unknown"
)

// RunState is the combined, normalized state of a Run.
// Invariants (enforced by Transition):
//   - terminal => Outcome is set, non-terminal => Outcome is zero
//   - suspended => SuspensionReason is set, active => SuspensionReason is zero
type RunState struct {
	Lifecycle        Lifecycle
	Phase            Phase
	Outcome          Outcome
	SuspensionReason SuspensionReason
}

// ValidPhases returns the set of legal phases.
func ValidPhases() map[Phase]bool {
	return map[Phase]bool{
		PhasePreparing:        true,
		PhaseCallingModel:     true,
		PhaseAwaitingApproval: true,
		PhaseExecutingTools:   true,
		PhaseCompacting:       true,
	}
}

// ValidOutcomes returns the set of legal outcomes.
func ValidOutcomes() map[Outcome]bool {
	return map[Outcome]bool{
		OutcomeSucceeded:           true,
		OutcomeCompletedUnverified: true,
		OutcomeNeedsUser:           true,
		OutcomeBudgetExhausted:     true,
		OutcomeFailed:              true,
		OutcomeCancelled:           true,
	}
}

// Validate checks invariants on RunState.
func (s RunState) Validate() error {
	switch s.Lifecycle {
	case LifecycleActive:
		if s.Outcome != "" {
			return fmt.Errorf("active run must not have outcome, got %q", s.Outcome)
		}
		if s.SuspensionReason != "" {
			return fmt.Errorf("active run must not have suspension reason, got %q", s.SuspensionReason)
		}
		if !ValidPhases()[s.Phase] {
			return fmt.Errorf("invalid phase %q", s.Phase)
		}
	case LifecycleSuspended:
		if s.Outcome != "" {
			return fmt.Errorf("suspended run must not have outcome, got %q", s.Outcome)
		}
		if s.SuspensionReason == "" {
			return fmt.Errorf("suspended run must have a suspension reason")
		}
	case LifecycleTerminal:
		if s.Outcome == "" {
			return fmt.Errorf("terminal run must have an outcome")
		}
		if !ValidOutcomes()[s.Outcome] {
			return fmt.Errorf("invalid outcome %q", s.Outcome)
		}
		if s.SuspensionReason != "" {
			return fmt.Errorf("terminal run must not have suspension reason, got %q", s.SuspensionReason)
		}
	default:
		return fmt.Errorf("invalid lifecycle %q", s.Lifecycle)
	}
	return nil
}

// transition defines a legal phase-to-phase or phase-to-terminal transition.
type transition struct {
	From Phase
	To   Phase // zero for terminal transitions
	Term bool  // if true, the transition leads to terminal
}

// legalTransitions enumerates every legal phase transition per §25.1.
var legalTransitions = []transition{
	{From: PhasePreparing, To: PhaseCallingModel},
	{From: PhaseCallingModel, To: PhaseAwaitingApproval},
	{From: PhaseCallingModel, To: PhaseExecutingTools},
	{From: PhaseCallingModel, To: PhasePreparing}, // response without tools, continue convergence
	{From: PhaseCallingModel, Term: true},         // response without tools, complete
	{From: PhaseAwaitingApproval, To: PhaseExecutingTools},
	{From: PhaseAwaitingApproval, To: PhasePreparing}, // denied
	{From: PhaseExecutingTools, To: PhaseCompacting},
	{From: PhaseExecutingTools, To: PhasePreparing},
	{From: PhaseCompacting, To: PhasePreparing},
}

// CanTransition reports whether a phase transition is legal.
func CanTransition(from, to Phase) bool {
	if from == to {
		return false
	}
	for _, t := range legalTransitions {
		if t.From == from && !t.Term && t.To == to {
			return true
		}
	}
	return false
}

// CanTerminate reports whether the run can transition to terminal from the given phase.
func CanTerminate(from Phase) bool {
	for _, t := range legalTransitions {
		if t.From == from && t.Term {
			return true
		}
	}
	// Any non-terminal phase can be cancelled or budget-exhausted
	return true
}

// Transition applies a phase transition, returning the new RunState.
func (s RunState) Transition(to Phase) (RunState, error) {
	if s.Lifecycle != LifecycleActive {
		return s, fmt.Errorf("cannot transition non-active run (lifecycle=%s)", s.Lifecycle)
	}
	if !CanTransition(s.Phase, to) {
		return s, fmt.Errorf("illegal transition: %s -> %s", s.Phase, to)
	}
	return RunState{Lifecycle: LifecycleActive, Phase: to}, nil
}

// Terminate moves the run to a terminal state with the given outcome.
func (s RunState) Terminate(outcome Outcome) (RunState, error) {
	if s.Lifecycle == LifecycleTerminal {
		return s, fmt.Errorf("run already terminal")
	}
	if !ValidOutcomes()[outcome] {
		return s, fmt.Errorf("invalid outcome %q", outcome)
	}
	return RunState{Lifecycle: LifecycleTerminal, Outcome: outcome}, nil
}

// Suspend moves the run to a suspended state with the given reason.
func (s RunState) Suspend(reason SuspensionReason) (RunState, error) {
	if s.Lifecycle == LifecycleTerminal {
		return s, fmt.Errorf("cannot suspend terminal run")
	}
	if reason == "" {
		return s, fmt.Errorf("suspension reason required")
	}
	return RunState{Lifecycle: LifecycleSuspended, Phase: s.Phase, SuspensionReason: reason}, nil
}

// Resume moves a suspended run back to active at the same phase.
func (s RunState) Resume() (RunState, error) {
	if s.Lifecycle != LifecycleSuspended {
		return s, fmt.Errorf("can only resume suspended run, got lifecycle=%s", s.Lifecycle)
	}
	return RunState{Lifecycle: LifecycleActive, Phase: s.Phase}, nil
}
