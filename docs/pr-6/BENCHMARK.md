# PDF text hit-test benchmark

Both runs compare the same original extraction algorithm and cached helper on the same fixture. Hardware and library differences mean timings across runs are not directly comparable.

## Fresh run — 2026-10-23

| Round | Queries per implementation | Original (µs/query) | Cached, warm (µs/query) |
| --- | ---: | ---: | ---: |
| 1 | 2,000 | 0.2855 | 0.017 |
| 2 | 2,000 | 0.3445 | 0.008 |
| 3 | 2,000 | 0.301 | 0.0085 |
| 4 | 2,000 | 0.293 | 0.008 |
| 5 | 2,000 | 0.3035 | 0.0085 |
| **Median** | | **0.301** | **0.0085** |

First cached query: **552 µs**, measured separately and excluded from warm timings. Warm median ratio: approximately **35.4×** for this fixture.

[Raw log](validation/pdf-hit-benchmark-current.log)

## Supplied validation — 2026-10-22

| Round | Queries per implementation | Original (µs/query) | Cached, warm (µs/query) |
| --- | ---: | ---: | ---: |
| 1 | 2,000 | 1.23 | 0.013 |
| 2 | 2,000 | 0.926 | 0.0125 |
| 3 | 2,000 | 0.769 | 0.013 |
| 4 | 2,000 | 1.5855 | 0.0125 |
| 5 | 2,000 | 0.8 | 0.0265 |
| **Median** | | **0.926** | **0.013** |

First cached query: **1865 µs**, measured separately and excluded from warm timings. Warm median ratio: approximately **71.2×** for this fixture.

[Raw log](validation/pdf-hit-benchmark.log)

## Fresh-run environment

- Debian 13, x86_64, AMD EPYC 9V74 host; container CPU quota: 2 cores.
- GCC 14.2, Release build (`-O3`), GTK 3.24.49, GLib 2.84.4, Poppler 25.03.0.
- GoogleTest built from the Debian source package with C++20.
- Plugins enabled; optional audio, QPDF, GtkSourceView, and cpptrace disabled in this capture build.

## Method and limits

- Both implementations query identical alternating hit/miss coordinates in the first page of `test/files/packaged_xopp/pdfBackground/old.xopp.bg.pdf`.
- Each implementation must return 1,000 hits from 2,000 queries per round.
- Execution order alternates across five paired rounds. The original path extracts glyph layout on every query; the cached helper reuses it.
- Timing uses `g_get_monotonic_time()`. Short cached queries are timed in batches; absolute results depend on hardware, libraries, and fixture.
- This small-fixture microbenchmark does not measure overall application speed, page loading, rendering, memory use, or cache invalidation cost.
- The original supplied log does not record CPU or OS details.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_GTEST=ON -DENABLE_CPPTRACE=OFF
cmake --build build --target benchmarks --parallel 8
build/test/benchmarks --gtest_filter='PdfTextHitBenchmark.*'
```

[Benchmark source](../../test/benchmarks/pdfTextHitBenchmark.cpp)
