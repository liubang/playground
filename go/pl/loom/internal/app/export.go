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

// export.go — session-log export: the raw event log as NDJSON for
// offline analysis and cross-tool trace comparison.

package app

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
	"github.com/liubang/playground/go/pl/loom/internal/session"
)

// ExportEvents returns the session's full persisted event log in sequence
// order; unknown sessions surface ErrSessionNotFound (a clean 404 on the
// wire rather than the store's typed invalid-input error).
func (s *SessionService) ExportEvents(ctx context.Context, id domain.SessionID) ([]domain.Event, error) {
	sqlite, ok := s.proc.Store.(*session.SQLiteStore)
	if !ok {
		return nil, fmt.Errorf("export is unavailable for this store")
	}
	insp, err := sqlite.InspectSession(ctx, id)
	if err != nil {
		var ae *domain.AgentError
		if errors.As(err, &ae) && ae.Code == domain.ErrInvalidInput &&
			strings.Contains(ae.Message, "session not found") {
			return nil, ErrSessionNotFound
		}
		return nil, err
	}
	return insp.Events, nil
}
