# FrameFlow — project rules for AI assistants

## What this is
FrameFlow: a C++17 latency-aware video pipeline for Linux (Ubuntu 24.04).
Paced source -> bounded queue (with overload policy) -> worker -> sink, plus metrics.
Specs: docs/PRD.md, docs/SPEC.md, docs/SRS.md, docs/ARCHITECTURE.md, docs/UIUX.md.
The SRS requirement IDs (FR-*, BR-*, VR-*, ER-*, EC-*) are the source of truth.
The phase plan is docs/SPEC.md section 16.

## Hard rules
- Implement ONLY the current phase I name. Do not add features from later phases.
- C++17, CMake, OpenCV, GoogleTest, CLI11, yaml-cpp, nlohmann-json. No other deps without asking.
- No raw new/delete. Ownership via std::unique_ptr. Frames are move-only.
- Concurrency: std::thread, std::mutex, std::condition_variable (always with a predicate), std::atomic. No lock-free code.
- No file I/O or logging on the source/worker threads.
- Use std::chrono::steady_clock only, via the injectable Clock class.
- Every new class: header comment stating responsibility and thread-safety.
- Every change comes with GoogleTest tests. Run them before saying you're done.
- Never invent benchmark numbers in docs or README.

## How to work with me (I am a student learning this)
- Before writing code, give a short plan: files, classes, and why. Wait for "go".
- Keep functions short. Prefer clear code over clever code.
- After each step, explain the key design decisions in 5-10 bullet points,
  including what could go wrong (races, deadlocks, lifetime bugs).
- If something in the specs is ambiguous, ask instead of guessing.

## Build & test
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
TSan: cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
