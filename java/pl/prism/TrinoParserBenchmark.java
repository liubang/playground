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
// Created: 2026/09/14 20:10

// Trino parser benchmark, for comparison with the Prism C++ pipeline
// benchmark (//cpp/pl/prism/benchmark:parser_benchmark). Reads the exact same
// synthetic workload .sql files (generated and checked in under
// cpp/pl/prism/benchmark/workloads/) and measures:
//   parse         -> SqlParser.createStatement
//   parse+format  -> SqlParser.createStatement + SqlFormatter.formatSql
//
// Trino does not expose its ANTLR lexer standalone, and there is no logical
// plan stage in trino-parser, so only these two stages are comparable.
//
// Run:
//   bazel run //java/pl/prism:trino_parser_benchmark
package pl.prism;

import com.google.devtools.build.runfiles.Runfiles;
import io.trino.sql.SqlFormatter;
import io.trino.sql.parser.SqlParser;
import io.trino.sql.tree.Statement;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

public final class TrinoParserBenchmark {

  private static final String[] WORKLOAD_NAMES = {
    "dashboard_s",
    "dashboard_m",
    "dashboard_l",
    "union_report_m",
    "union_report_l",
    "expr_deep",
    "subquery_nest",
  };

  private static final long WARMUP_NANOS = 1_000_000_000L;
  private static final long EPOCH_NANOS = 200_000_000L;
  private static final int EPOCHS = 5;

  // Prevents the JIT from eliminating the measured calls.
  private static long sink;

  private record Workload(String name, String sql) {}

  private interface Op {
    void run();
  }

  public static void main(String[] args) throws Exception {
    final List<Workload> workloads = loadWorkloads();

    System.out.println(
        "java.version="
            + System.getProperty("java.version")
            + " java.vendor="
            + System.getProperty("java.vendor")
            + " trino-parser=468");
    System.out.println("| workload | bytes |");
    System.out.println("|---|---:|");
    for (Workload w : workloads) {
      System.out.println("| " + w.name() + " | " + w.sql().length() + " |");
    }
    System.out.println();

    final SqlParser parser = new SqlParser();
    for (Workload w : workloads) {
      validate(parser, w);
    }

    System.out.println(
        "| workload | parse ns/B | parse MB/s | parse+format ns/B | parse+format MB/s |");
    System.out.println("|---|---:|---:|---:|---:|");
    for (Workload w : workloads) {
      final double bytes = w.sql().length();
      final double parseNs =
          measureNanosPerOp(
              () -> {
                Statement stmt = parser.createStatement(w.sql());
                sink += stmt.getChildren().size();
              });
      final double formatNs =
          measureNanosPerOp(
              () -> {
                Statement stmt = parser.createStatement(w.sql());
                sink += SqlFormatter.formatSql(stmt).length();
              });
      System.out.println(
          String.format(
              Locale.ROOT,
              "| %s | %.2f | %.1f | %.2f | %.1f |",
              w.name(),
              parseNs / bytes,
              mbPerSec(parseNs, bytes),
              formatNs / bytes,
              mbPerSec(formatNs, bytes)));
    }
    System.out.println("sink=" + sink);
  }

  private static List<Workload> loadWorkloads() throws IOException {
    final Runfiles runfiles = Runfiles.preload().withSourceRepository("");
    final List<Workload> workloads = new ArrayList<>();
    for (String name : WORKLOAD_NAMES) {
      final String path =
          runfiles.rlocation("playground/cpp/pl/prism/benchmark/workloads/" + name + ".sql");
      workloads.add(new Workload(name, Files.readString(Path.of(path))));
    }
    return workloads;
  }

  private static void validate(SqlParser parser, Workload w) {
    final Statement stmt = parser.createStatement(w.sql());
    if (SqlFormatter.formatSql(stmt).isEmpty()) {
      throw new IllegalStateException("workload " + w.name() + " formatted to empty string");
    }
  }

  private static double mbPerSec(double nanosPerOp, double bytesPerOp) {
    return bytesPerOp * 1000.0 / nanosPerOp;
  }

  private static double measureNanosPerOp(Op op) {
    // Warmup: let the JIT settle, and estimate the per-op cost.
    final long warmupStart = System.nanoTime();
    long warmupIters = 0;
    while (System.nanoTime() - warmupStart < WARMUP_NANOS) {
      op.run();
      ++warmupIters;
    }
    final double estNsPerOp = (System.nanoTime() - warmupStart) / (double) warmupIters;
    final int itersPerEpoch = Math.max(1, (int) (EPOCH_NANOS / estNsPerOp));

    final double[] epochs = new double[EPOCHS];
    for (int e = 0; e < EPOCHS; ++e) {
      final long start = System.nanoTime();
      for (int i = 0; i < itersPerEpoch; ++i) {
        op.run();
      }
      epochs[e] = (System.nanoTime() - start) / (double) itersPerEpoch;
    }
    Arrays.sort(epochs);
    return epochs[EPOCHS / 2];
  }

  private TrinoParserBenchmark() {}
}
