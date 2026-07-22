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

// ErrorCode is a stable error classification across modules.
type ErrorCode string

const (
	ErrInvalidInput ErrorCode = "invalid_input"
	ErrPermission   ErrorCode = "permission"
	ErrConflict     ErrorCode = "conflict"
	ErrUnavailable  ErrorCode = "unavailable"
	ErrRateLimited  ErrorCode = "rate_limited"
	ErrTimeout      ErrorCode = "timeout"
	ErrCancelled    ErrorCode = "cancelled"
	ErrBudget       ErrorCode = "budget"
	ErrSecurity     ErrorCode = "security"
	ErrInternal     ErrorCode = "internal"
)

// AgentError is the standard error type for the agent runtime.
type AgentError struct {
	Code      ErrorCode
	Message   string
	Retryable bool
	Cause     error
}

func (e *AgentError) Error() string {
	if e.Cause != nil {
		return fmt.Sprintf("[%s] %s: %v", e.Code, e.Message, e.Cause)
	}
	return fmt.Sprintf("[%s] %s", e.Code, e.Message)
}

func (e *AgentError) Unwrap() error { return e.Cause }

// NewError creates a new AgentError.
func NewError(code ErrorCode, msg string, opts ...ErrorOpt) *AgentError {
	e := &AgentError{Code: code, Message: msg}
	for _, o := range opts {
		o(e)
	}
	return e
}

// ErrorOpt configures an AgentError.
type ErrorOpt func(*AgentError)

func WithRetryable(r bool) ErrorOpt  { return func(e *AgentError) { e.Retryable = r } }
func WithCause(cause error) ErrorOpt { return func(e *AgentError) { e.Cause = cause } }

// IsRetryable reports whether an error is retryable.
func IsRetryable(err error) bool {
	var ae *AgentError
	if As(err, &ae) {
		return ae.Retryable
	}
	return false
}

// As is a convenience wrapper around the same pattern as errors.As.
func As(err error, target any) bool {
	for err != nil {
		if e, ok := err.(interface{ As(any) bool }); ok {
			return e.As(target)
		}
		// simple type check
		if t, ok := target.(**AgentError); ok {
			if ae, ok := err.(*AgentError); ok {
				*t = ae
				return true
			}
		}
		if unwrapper, ok := err.(interface{ Unwrap() error }); ok {
			err = unwrapper.Unwrap()
		} else {
			return false
		}
	}
	return false
}
