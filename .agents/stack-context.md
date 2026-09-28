# Stack Context

Generated: 2026-09-27

## Stack
- Language: C++20; C-style LEAP sources compiled as C++.
- Framework: none; native CLI and static libraries.
- Build: CMake >= 3.20, Threads; vendored Edlib and optional AVX2 LEAP.
- Local CMake: build-tools/cmake/data/bin/cmake (system CMake is too old).
- Test: build-tools/cmake/data/bin/ctest --test-dir <build> --output-on-failure.
- Lint/format: no configured gate found.

## Secondary Languages
- Python: synthetic query generation.
- CMake: CLI regression test.

## Conventions
- Public headers: include/navigamer; implementations: src.
- Naming: PascalCase types, snake_case functions and variables.
- Error handling: exceptions at public/CLI boundaries.
- Tests: assert-based C++ tests, independent DP oracle, CTest CLI tests.
- Keep AVX2-only code in separate translation units with CPU dispatch.
- Preserve index serialization and deterministic member/center order.

## CI Gates
- No repository CI workflow found; use CTest and controlled benchmarks.
