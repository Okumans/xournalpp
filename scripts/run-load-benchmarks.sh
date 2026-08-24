#!/usr/bin/env bash

set -euo pipefail

benchmark_binary=${1:-build/test/benchmarks}

if [[ ! -x ${benchmark_binary} ]]; then
    echo "Benchmark binary is not executable: ${benchmark_binary}" >&2
    echo "Configure with -DENABLE_GTEST=ON and build the benchmarks target first." >&2
    exit 1
fi

# Each case runs in a fresh process so peak_rss_kib is isolated. Override the
# built-in iteration counts with XOPP_BENCHMARK_ITERATIONS when doing a smoke run.
benchmark_cases=(
    benchmarkHandwrittenText
    benchmarkTypedText
    benchmarkLatex
    benchmarkEmpty
    benchmarkManyPages
)

for benchmark_case in "${benchmark_cases[@]}"; do
    "${benchmark_binary}" \
        --gtest_color=no \
        --gtest_filter="FileLoadBenchmark.${benchmark_case}"
done
