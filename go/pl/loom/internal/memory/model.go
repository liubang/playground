// Copyright (c) 2026 The Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package memory

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"unicode/utf8"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

const maxModelTextBytes = 128 * 1024

// completeText accepts only a complete, normally terminated text response.
// Reading through EOF catches transport errors even after response_end.
func completeText(ctx context.Context, model domain.Model, req domain.ModelRequest) (text string, err error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if model == nil {
		return "", errors.New("no model available")
	}
	stream, err := model.Stream(ctx, req)
	if err != nil {
		return "", err
	}
	if stream == nil {
		return "", errors.New("model returned a nil stream")
	}
	defer func() {
		err = errors.Join(err, stream.Close(), ctx.Err())
		if err != nil {
			text = ""
		}
	}()

	var output strings.Builder
	terminal := false
	for {
		if err := ctx.Err(); err != nil {
			return "", err
		}
		event, recvErr := stream.Recv()
		if err := ctx.Err(); err != nil {
			return "", err
		}
		if recvErr != nil {
			if !errors.Is(recvErr, io.EOF) {
				return "", recvErr
			}
			if !terminal {
				return "", errors.New("model stream ended without response_end")
			}
			if !utf8.ValidString(output.String()) {
				return "", errors.New("model output is not valid UTF-8")
			}
			text = RedactSecrets(output.String())
			if len(text) > maxModelTextBytes {
				return "", errors.New("redacted model output exceeds text limit")
			}
			return text, nil
		}
		switch event.Kind {
		case domain.ModelEventTextDelta:
			if terminal {
				return "", errors.New("model text received after response_end")
			}
			if len(event.TextDelta) > maxModelTextBytes-output.Len() {
				return "", errors.New("model output exceeds text limit")
			}
			output.WriteString(event.TextDelta)
		case domain.ModelEventStreamError:
			if event.Error == "" {
				return "", errors.New("unknown model stream error")
			}
			return "", errors.New(event.Error)
		case domain.ModelEventResponseEnd:
			if terminal {
				return "", errors.New("duplicate model response_end")
			}
			if event.StopReason != domain.StopEndTurn {
				return "", fmt.Errorf("model stopped abnormally: %q", event.StopReason)
			}
			terminal = true
		case domain.ModelEventToolCallStart, domain.ModelEventToolArgsDelta, domain.ModelEventToolCallEnd:
			return "", errors.New("unexpected tool call in model text response")
		}
	}
}
