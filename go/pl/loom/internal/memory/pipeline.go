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
// Created: 2026/08/06

package memory

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// PipelineConfig tunes the background memory pipeline. The zero value is
// ready to use; see DefaultPipelineConfig.
type PipelineConfig struct {
	// MaxJobsPerRun bounds Phase 1 jobs claimed per pass.
	MaxJobsPerRun int
	// Concurrency bounds parallel extraction model calls within a pass.
	Concurrency int
	// MinIdle skips sessions touched more recently than this.
	MinIdle time.Duration
	// MaxAge skips sessions last touched longer ago than this.
	MaxAge time.Duration
	// Lease bounds a Phase 1 claim; a crashed worker's jobs become
	// claimable again once the lease expires.
	Lease time.Duration
	// RetryDelay is the base backoff between failed extraction attempts
	// (linear: RetryDelay * attempts).
	RetryDelay time.Duration
	// MaxAttempts bounds extraction attempts before a job is abandoned.
	MaxAttempts int
	// JobTimeout bounds one extraction model call.
	JobTimeout time.Duration
	// Phase2Lease bounds the global consolidation lease.
	Phase2Lease time.Duration
	// ConsolidateTimeout bounds one Phase 2 consolidation pass.
	ConsolidateTimeout time.Duration
}

// DefaultPipelineConfig returns the built-in pipeline tuning.
func DefaultPipelineConfig() PipelineConfig {
	return PipelineConfig{
		MaxJobsPerRun:      8,
		Concurrency:        4,
		MinIdle:            time.Hour,
		MaxAge:             30 * 24 * time.Hour,
		Lease:              time.Hour,
		RetryDelay:         30 * time.Minute,
		MaxAttempts:        5,
		JobTimeout:         3 * time.Minute,
		Phase2Lease:        30 * time.Minute,
		ConsolidateTimeout: 5 * time.Minute,
	}
}

// PipelineStats summarizes one pipeline pass.
type PipelineStats struct {
	Claimed       int
	Succeeded     int
	NoOutput      int
	Failed        int
	Phase2Ran     bool
	Phase2Changed bool
	Phase2Skipped bool // lease held by another process
}

// Pipeline is the background memory worker: it drains the Phase 1 job
// queue (crash-safe, leased, with retry backoff) and then runs Phase 2
// consolidation under the global lease. It runs at process startup and on
// an interval afterwards, never on the session/process shutdown path.
type Pipeline struct {
	jobs         JobQueue
	sessions     domain.SessionStore
	extractor    *Extractor
	consolidator *Consolidator
	cfg          PipelineConfig
	logger       *slog.Logger
}

// NewPipeline assembles the background memory worker. sessions is used to
// load claimed sessions' checkpoints; jobs persists queue state.
func NewPipeline(jobs JobQueue, sessions domain.SessionStore, extractor *Extractor, consolidator *Consolidator, cfg PipelineConfig, logger *slog.Logger) *Pipeline {
	if logger == nil {
		logger = slog.Default()
	}
	return &Pipeline{
		jobs:         jobs,
		sessions:     sessions,
		extractor:    extractor,
		consolidator: consolidator,
		cfg:          cfg,
		logger:       logger,
	}
}

// Start launches the pipeline loop in the background: one immediate pass,
// then a pass every interval (interval <= 0 runs only the startup pass).
// The loop stops when ctx is cancelled; in-flight claims simply expire.
func (p *Pipeline) Start(ctx context.Context, interval time.Duration) {
	go func() {
		p.RunOnce(ctx)
		if interval <= 0 {
			return
		}
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				p.RunOnce(ctx)
			}
		}
	}()
}

// RunOnce executes one pipeline pass: Phase 1 extraction for claimed jobs
// (bounded concurrency), then Phase 2 consolidation under the global
// lease. All errors are logged and swallowed into stats — the pipeline is
// best-effort background work and must never disturb the foreground.
func (p *Pipeline) RunOnce(ctx context.Context) PipelineStats {
	var stats PipelineStats
	if p == nil || p.jobs == nil {
		return stats
	}
	stats = p.runPhase1(ctx)
	p.runPhase2(ctx, &stats)
	if stats.Claimed > 0 || stats.Phase2Ran {
		p.logger.Info("memory pipeline pass complete",
			"claimed", stats.Claimed,
			"succeeded", stats.Succeeded,
			"no_output", stats.NoOutput,
			"failed", stats.Failed,
			"phase2_ran", stats.Phase2Ran,
			"phase2_changed", stats.Phase2Changed)
	}
	return stats
}

func (p *Pipeline) runPhase1(ctx context.Context) PipelineStats {
	var stats PipelineStats
	if p.extractor == nil {
		return stats
	}
	jobs, err := p.jobs.ClaimMemoryJobs(ctx, p.cfg.MaxJobsPerRun, p.cfg.MinIdle, p.cfg.MaxAge, p.cfg.Lease, p.cfg.MaxAttempts)
	if err != nil {
		p.logger.Warn("memory pipeline: claim jobs failed", "error", err)
		return stats
	}
	stats.Claimed = len(jobs)
	if len(jobs) == 0 {
		return stats
	}

	concurrency := p.cfg.Concurrency
	if concurrency <= 0 {
		concurrency = 1
	}
	sem := make(chan struct{}, concurrency)
	var mu sync.Mutex
	var wg sync.WaitGroup
	for _, job := range jobs {
		if ctx.Err() != nil {
			break
		}
		job := job
		wg.Add(1)
		go func() {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			outcome := p.processJob(ctx, job)
			mu.Lock()
			switch outcome {
			case JobSucceeded:
				stats.Succeeded++
			case JobSucceededNoOutput:
				stats.NoOutput++
			default:
				stats.Failed++
			}
			mu.Unlock()
		}()
	}
	wg.Wait()
	return stats
}

// settleContext bounds queue settlement writes so a stuck database cannot
// pin a worker goroutine during process exit.
func settleContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), 10*time.Second)
}

// processJob runs Phase 1 for one claimed job and settles the claim.
func (p *Pipeline) processJob(ctx context.Context, job Job) JobStatus {
	fail := func(err error) JobStatus {
		p.logger.Warn("memory extraction failed",
			"session", job.SessionID, "error", err)
		sctx, cancel := settleContext()
		defer cancel()
		if ferr := p.jobs.FailMemoryJob(sctx, job.SessionID, job.ClaimToken, err, p.cfg.RetryDelay, p.cfg.MaxAttempts); ferr != nil {
			p.logger.Warn("memory job failure update failed",
				"session", job.SessionID, "error", ferr)
		}
		return JobFailed
	}
	complete := func(status JobStatus) JobStatus {
		// Completion uses a fresh context: a cancelled pass must not lose
		// the settlement write, or the job would stall until lease expiry.
		sctx, cancel := settleContext()
		defer cancel()
		if err := p.jobs.CompleteMemoryJob(sctx, job.SessionID, job.ClaimToken, status, job.SessionVersion); err != nil {
			p.logger.Warn("memory job completion update failed",
				"session", job.SessionID, "error", err)
			return JobFailed
		}
		return status
	}

	if ctx.Err() != nil {
		return JobFailed // pass cancelled; the lease expires and is reclaimed
	}

	jobCtx, cancel := context.WithTimeout(ctx, p.cfg.JobTimeout)
	defer cancel()

	ckpt, err := p.sessions.LoadLatestCheckpoint(jobCtx, job.SessionID)
	if err != nil {
		// "No checkpoint" (session created but never completed a turn) is a
		// terminal no-output; any other failure (DB errors, decode
		// failures) is retryable — settling it as no-output would lose the
		// session's memory forever.
		var ae *domain.AgentError
		if errors.As(err, &ae) && ae.Code == domain.ErrInvalidInput {
			return complete(JobSucceededNoOutput)
		}
		return fail(err)
	}
	if len(ckpt.Messages) == 0 {
		return complete(JobSucceededNoOutput)
	}

	result, err := p.extractor.ExtractFromSession(jobCtx, job.SessionID, ckpt.Messages, job.WorkspaceRoot)
	if err != nil {
		return fail(err)
	}
	if result == nil {
		return complete(JobSucceededNoOutput)
	}
	p.logger.Info("memory extraction completed",
		"session", job.SessionID, "slug", result.RolloutSlug)
	return complete(JobSucceeded)
}

func (p *Pipeline) runPhase2(ctx context.Context, stats *PipelineStats) {
	if p.consolidator == nil {
		return
	}
	token := newLeaseToken()
	acquired, err := p.jobs.AcquirePhase2Lease(ctx, token, p.cfg.Phase2Lease)
	if err != nil {
		p.logger.Warn("memory pipeline: phase2 lease acquisition failed", "error", err)
		return
	}
	if !acquired {
		stats.Phase2Skipped = true
		return
	}
	defer func() {
		sctx, cancel := settleContext()
		defer cancel()
		if err := p.jobs.ReleasePhase2Lease(sctx, token); err != nil {
			p.logger.Warn("memory pipeline: phase2 lease release failed", "error", err)
		}
	}()

	cctx, cancel := context.WithTimeout(ctx, p.cfg.ConsolidateTimeout)
	defer cancel()
	changed, err := p.consolidator.Consolidate(cctx)
	if err != nil {
		p.logger.Warn("memory consolidation failed", "error", err)
		return
	}
	stats.Phase2Ran = true
	stats.Phase2Changed = changed
}

func newLeaseToken() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand never fails on supported platforms; fall back to a
		// time-based token rather than failing the pass.
		return time.Now().UTC().Format("lease-20060102150405.000000000")
	}
	return hex.EncodeToString(b[:])
}
