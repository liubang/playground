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
// Created: 2026/07/25

package trace

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"time"
	"unicode/utf8"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	oteltrace "go.opentelemetry.io/otel/trace"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// Attribute keys. Langfuse v3 maps its own observation attributes onto the
// trace data model, and additionally understands the OpenTelemetry GenAI
// semantic conventions; emitting both keeps the data usable in any
// OTLP-compatible backend while rendering as first-class generations in
// Langfuse.
const (
	attrObservationType   = "langfuse.observation.type"
	attrObservationInput  = "langfuse.observation.input"
	attrObservationOutput = "langfuse.observation.output"
	attrObservationModel  = "langfuse.observation.model.name"
	attrObservationUsage  = "langfuse.observation.usage_details"

	attrTraceName      = "langfuse.trace.name"
	attrTraceInput     = "langfuse.trace.input"
	attrTraceOutput    = "langfuse.trace.output"
	attrTraceMetadata  = "langfuse.trace.metadata"
	attrTraceTags      = "langfuse.trace.tags"
	attrTraceSessionID = "session.id"
	attrTraceUserID    = "user.id"
	attrTraceRelease   = "langfuse.release"

	// Compat keys kept for older Langfuse deployments (see the OTel property
	// mapping note in packages/shared/src/server/otel/attributes.ts).
	attrTraceCompatSessionID = "langfuse.session.id"
	attrTraceCompatUserID    = "langfuse.user.id"

	attrObservationCost          = "langfuse.observation.cost_details"
	attrObservationPromptName    = "langfuse.observation.prompt.name"
	attrObservationPromptVersion = "langfuse.observation.prompt.version"

	attrEnvironment = "langfuse.environment"

	attrGenAISystem         = "gen_ai.system"
	attrGenAIRequestModel   = "gen_ai.request.model"
	attrGenAIInputTokens    = "gen_ai.usage.input_tokens"
	attrGenAIOutputTokens   = "gen_ai.usage.output_tokens"
	attrGenAIFinishReasons  = "gen_ai.response.finish_reasons"
	attrGenAIInputMessages  = "gen_ai.input.messages"
	attrGenAIOutputMessages = "gen_ai.output.messages"
)

// otlpPath is the Langfuse v3 OTLP traces endpoint below the base host.
const otlpPath = "/api/public/otel/v1/traces"

// maxAttributeContent bounds any single JSON attribute written to a span.
// Langfuse rejects oversized events and huge attributes make traces
// unwieldy; overflow is marked with a truncation suffix.
const maxAttributeContent = 256 << 10

// Provider owns the OTLP exporter and tracer provider. Close it with
// Shutdown to flush buffered spans.
type Provider struct {
	tp       *sdktrace.TracerProvider
	recorder *otelRecorder
	scores   *scoreClient
}

// Setup builds the OTLP exporter and tracer provider. A disabled Config is
// an error here — callers should skip Setup and use Noop instead.
func Setup(ctx context.Context, cfg Config) (*Provider, error) {
	if !cfg.Enabled {
		return nil, fmt.Errorf("trace.Setup: config disabled (host and keys required)")
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.DiscardHandler)
	}
	// The OTLP SDK's default error handler prints to stderr, which tears
	// the TUI's rendering. Route exporter failures (serialization errors,
	// network, 4xx) into the injected logger instead — discardable in the
	// TUI, visible in headless runs.
	otel.SetErrorHandler(otel.ErrorHandlerFunc(func(err error) {
		logger.Warn("otel traces export failed", "error", err)
	}))
	exporter, err := otlptracehttp.New(ctx,
		otlptracehttp.WithEndpointURL(cfg.Host+otlpPath),
		otlptracehttp.WithHeaders(map[string]string{"Authorization": basicAuthHeader(cfg.PublicKey, cfg.SecretKey)}),
	)
	if err != nil {
		return nil, fmt.Errorf("trace.Setup: create OTLP exporter: %w", err)
	}
	res, err := resource.New(ctx, resource.WithAttributes(
		attribute.String("service.name", "loom"),
		attribute.String(attrEnvironment, cfg.Environment),
	))
	if err != nil {
		// Do not leak the exporter on the error path.
		_ = exporter.Shutdown(ctx)
		return nil, fmt.Errorf("trace.Setup: create resource: %w", err)
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exporter),
		sdktrace.WithResource(res),
	)
	scores := newScoreClient(cfg.Host, cfg.PublicKey, cfg.SecretKey, cfg.Environment, logger)
	return &Provider{
		tp: tp,
		recorder: &otelRecorder{
			tracer:  tp.Tracer("loom.agent"),
			content: cfg.IncludeContent,
			userID:  cfg.UserID,
			release: cfg.Release,
			costIn:  cfg.CostInputPerMTok,
			costOut: cfg.CostOutputPerMTok,
			scores:  scores,
		},
		scores: scores,
	}, nil
}

// Recorder returns the agent-facing recorder backed by this provider.
func (p *Provider) Recorder() Recorder { return p.recorder }

// Shutdown flushes buffered spans and releases the exporter, then waits
// (briefly, bounded) for in-flight score submissions: runs are scored at
// the very end, so without the wait a prompt process exit could drop the
// score even though the request was already queued.
func (p *Provider) Shutdown(ctx context.Context) error {
	err := p.tp.Shutdown(ctx)
	if p.scores != nil {
		p.scores.waitIdle(2 * time.Second)
	}
	return err
}

// otelRecorder exports spans over OTLP. Generation and tool spans are
// created post-hoc with backdated timestamps — the loop reports completed
// operations, which keeps instrumentation off the hot path.
type otelRecorder struct {
	tracer  oteltrace.Tracer
	content bool
	userID  string
	release string
	costIn  float64
	costOut float64
	scores  *scoreClient
}

type otelRun struct {
	span oteltrace.Span
	rec  *otelRecorder
}

func (r *otelRecorder) StartRun(ctx context.Context, meta RunMeta) (context.Context, RunHandle) {
	// Redaction hygiene (LOOM_TRACE_CONTENT=0): the workspace path is a
	// local filesystem path and stays out of tags entirely; the user id is
	// reported only as an irreversible hash so per-user success rates remain
	// computable without shipping an email address.
	var tags []string
	if r.content {
		tags = append(tags, meta.Workspace)
	}
	if r.release != "" {
		tags = append(tags, "v"+strings.TrimPrefix(r.release, "v"))
	}
	attrs := []attribute.KeyValue{
		attribute.String(attrTraceName, "loom.run"),
		attribute.String(attrTraceSessionID, meta.SessionID),
		attribute.String(attrTraceCompatSessionID, meta.SessionID),
		attribute.String(attrTraceMetadata, mustJSON(map[string]string{
			"run_id": meta.RunID,
			"model":  meta.Model,
		})),
	}
	userID := r.userID
	if !r.content {
		userID = hashUserID(userID)
	}
	if userID != "" {
		attrs = append(attrs,
			attribute.String(attrTraceUserID, userID),
			attribute.String(attrTraceCompatUserID, userID),
		)
	}
	if r.release != "" {
		attrs = append(attrs, attribute.String(attrTraceRelease, r.release))
	}
	attrs = append(attrs, attribute.StringSlice(attrTraceTags, compactStrings(tags)))
	if r.content && meta.Prompt != "" {
		attrs = append(attrs, attribute.String(attrTraceInput, truncateContent(meta.Prompt)))
	}
	ctx, span := r.tracer.Start(ctx, "loom.run", oteltrace.WithAttributes(attrs...))
	return ctx, &otelRun{span: span, rec: r}
}

func (run *otelRun) RecordGeneration(ctx context.Context, rec GenerationRecord) {
	attrs := []attribute.KeyValue{
		attribute.String(attrObservationType, "generation"),
		attribute.String(attrObservationModel, rec.Model),
		attribute.String(attrGenAISystem, "openai"),
		attribute.String(attrGenAIRequestModel, rec.Model),
		attribute.Int64(attrGenAIInputTokens, rec.InputTokens),
		attribute.Int64(attrGenAIOutputTokens, rec.OutputTokens),
		attribute.String(attrObservationUsage, mustJSON(map[string]int64{
			"input": rec.InputTokens, "output": rec.OutputTokens,
		})),
		attribute.String("loom.request_id", rec.RequestID),
		attribute.Int("loom.turn", rec.Turn),
	}
	if rec.StopReason != "" {
		attrs = append(attrs, attribute.StringSlice(attrGenAIFinishReasons, []string{rec.StopReason}))
	}
	if rec.PromptName != "" {
		attrs = append(attrs,
			attribute.String(attrObservationPromptName, rec.PromptName),
			attribute.Int(attrObservationPromptVersion, rec.PromptVersion),
		)
	}
	if run.rec.costIn > 0 && run.rec.costOut > 0 && (rec.InputTokens > 0 || rec.OutputTokens > 0) {
		attrs = append(attrs, attribute.String(attrObservationCost, mustJSON(map[string]float64{
			"input":  float64(rec.InputTokens) * run.rec.costIn / 1e6,
			"output": float64(rec.OutputTokens) * run.rec.costOut / 1e6,
		})))
	}
	attrs = append(attrs, attribute.String(attrObservationInput, encodeMessages(rec.Input, run.rec.content)))
	attrs = append(attrs, attribute.String(attrGenAIInputMessages, encodeMessages(rec.Input, run.rec.content)))
	if !rec.Output.ID.IsZero() {
		output := encodeMessages([]domain.Message{rec.Output}, run.rec.content)
		attrs = append(attrs,
			attribute.String(attrObservationOutput, output),
			attribute.String(attrGenAIOutputMessages, output),
		)
	}

	_, span := run.rec.tracer.Start(ctx, "gen_ai.chat",
		oteltrace.WithTimestamp(rec.StartTime),
		oteltrace.WithAttributes(attrs...),
	)
	if rec.Err != nil {
		span.RecordError(rec.Err, oteltrace.WithTimestamp(rec.EndTime))
		span.SetStatus(codes.Error, rec.Err.Error())
	}
	span.End(oteltrace.WithTimestamp(rec.EndTime))
}

func (run *otelRun) RecordTool(ctx context.Context, rec ToolRecord) {
	attrs := []attribute.KeyValue{
		attribute.String(attrObservationType, "span"),
		attribute.String("loom.tool.name", rec.Name),
		attribute.String("loom.tool.risk", rec.Risk),
		attribute.String("loom.tool.call_id", rec.CallID),
		attribute.String("loom.tool.status", rec.Status),
	}
	if run.rec.content {
		if len(rec.Arguments) > 0 {
			attrs = append(attrs, attribute.String(attrObservationInput, truncateContent(string(rec.Arguments))))
		}
		if rec.Preview != "" {
			attrs = append(attrs, attribute.String(attrObservationOutput, truncateContent(rec.Preview)))
		}
	}
	_, span := run.rec.tracer.Start(ctx, "tool."+rec.Name,
		oteltrace.WithTimestamp(rec.StartTime),
		oteltrace.WithAttributes(attrs...),
	)
	if rec.Status != "success" {
		// Under redaction the status message carries only the stable error
		// code; the free-form message may embed paths or file content.
		errText := rec.Error
		if !run.rec.content {
			errText = rec.Code
		}
		if errText != "" {
			span.SetStatus(codes.Error, sanitizeUTF8(errText))
		}
	}
	span.End(oteltrace.WithTimestamp(rec.EndTime))
}

func (run *otelRun) RecordEvent(_ context.Context, name string, attrs map[string]string) {
	kvs := make([]attribute.KeyValue, 0, len(attrs))
	for k, v := range attrs {
		kvs = append(kvs, attribute.String(k, sanitizeUTF8(v)))
	}
	run.span.AddEvent(name, oteltrace.WithAttributes(kvs...))
}

// Score reports a numeric trace score through Langfuse's scores API. The
// call is fire-and-forget: reporting must never block or fail the loop.
func (run *otelRun) Score(ctx context.Context, name string, value float64, comment string) {
	if run.rec.scores == nil {
		return
	}
	traceID := run.span.SpanContext().TraceID().String()
	run.rec.scores.submit(name, traceID, value, comment)
}

func (run *otelRun) End(result RunResult) {
	if run.rec.content && result.Output != "" {
		run.span.SetAttributes(attribute.String(attrTraceOutput, truncateContent(result.Output)))
	}
	if result.Error != "" {
		run.span.SetStatus(codes.Error, sanitizeUTF8(result.Error))
	} else {
		run.span.SetStatus(codes.Ok, result.Outcome)
	}
	run.span.End()
}

// summaryMessage is the wire form for one traced message: full text when
// content capture is on, structural summary otherwise.
type summaryMessage struct {
	Role  string        `json:"role"`
	Parts []summaryPart `json:"parts"`
}

type summaryPart struct {
	Kind  string `json:"kind"`
	Text  string `json:"text,omitempty"`
	Name  string `json:"name,omitempty"`  // tool_call name
	Args  string `json:"args,omitempty"`  // tool_call arguments (content on)
	Error string `json:"error,omitempty"` // tool_result error
	Bytes int    `json:"bytes,omitempty"` // content off: size instead of text
}

// encodeMessages renders messages as a bounded JSON array for span
// attributes. With content disabled only roles, part kinds and byte sizes
// are emitted, so no conversation text leaves the process.
func encodeMessages(messages []domain.Message, content bool) string {
	out := make([]summaryMessage, 0, len(messages))
	for _, msg := range messages {
		sm := summaryMessage{Role: string(msg.Role)}
		for _, p := range msg.Parts {
			sp := summaryPart{Kind: string(p.Kind)}
			switch {
			case p.Kind == domain.PartText:
				if content {
					sp.Text = truncateContent(p.Text)
				} else {
					sp.Bytes = len(p.Text)
				}
			case p.Kind == domain.PartToolCall && p.ToolCall != nil:
				sp.Name = p.ToolCall.Name
				if content {
					sp.Args = truncateContent(string(p.ToolCall.Arguments))
				} else {
					sp.Bytes = len(p.ToolCall.Arguments)
				}
			case p.Kind == domain.PartToolResult && p.ToolResult != nil:
				if content {
					var text string
					for _, cp := range p.ToolResult.Content {
						if cp.Kind == domain.PartText {
							text += cp.Text
						}
					}
					sp.Text = truncateContent(text)
				}
				if p.ToolResult.Error != nil {
					if content {
						sp.Error = p.ToolResult.Error.Message
					} else {
						// Redacted: only the stable classification crosses the
						// wire; messages may embed paths or file content.
						sp.Error = p.ToolResult.Error.Code
					}
				}
			}
			sm.Parts = append(sm.Parts, sp)
		}
		out = append(out, sm)
	}
	return mustJSON(out)
}

func mustJSON(v any) string {
	data, err := json.Marshal(v)
	if err != nil {
		return fmt.Sprintf(`{"encode_error":%q}`, err.Error())
	}
	return truncateContent(string(data))
}

// sanitizeUTF8 makes a string safe for OTLP span attributes: the protobuf
// encoder rejects invalid UTF-8, and the exporter's error handler would
// otherwise log the rejection (to stderr, tearing the TUI). Tool and model
// output may carry arbitrary bytes, so every field that crosses the wire is
// sanitized here (see truncateContent) or at emission.
func sanitizeUTF8(s string) string {
	return strings.ToValidUTF8(s, "�")
}

// truncateContent bounds a string for span attributes, marking truncation.
// Sanitizes first and backs off to a rune boundary, so the result is
// always valid UTF-8 and never shows a replacement char mid-text.
func truncateContent(s string) string {
	s = sanitizeUTF8(s)
	if len(s) > maxAttributeContent {
		cut := maxAttributeContent
		for cut > 0 && !utf8.ValidString(s[:cut]) {
			cut--
		}
		return s[:cut] + "…[truncated]"
	}
	return s
}

// hashUserID irreversibly hashes a user identifier for redacted traces.
func hashUserID(id string) string {
	if id == "" {
		return ""
	}
	sum := sha256.Sum256([]byte(id))
	return "u_" + hex.EncodeToString(sum[:])[:16]
}

// compactStrings drops empty entries.
func compactStrings(in []string) []string {
	out := in[:0]
	for _, s := range in {
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}
