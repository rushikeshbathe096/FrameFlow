# FrameFlow Design Notes (as built)

## Testing and sanitizers

**Why the tsan preset runs tests under `setarch -R`.** GCC 13's TSan runtime crashes at
startup on recent kernels with high ASLR entropy (`vm.mmap_rnd_bits`); even an empty `main()`
segfaults. The tsan preset sets `CMAKE_CROSSCOMPILING_EMULATOR` to `setarch;-R`, so ctest and
`gtest_discover_tests` run test binaries with ASLR disabled for that process only. No system
setting is changed. Alternatives: `sudo sysctl vm.mmap_rnd_bits=28` (machine-wide), or a
newer toolchain whose TSan runtime handles the higher entropy.

**Why `tsan.supp` suppresses only libtbb.** OpenCV uses TBB as its parallel backend. TBB is
not built with TSan, so TSan cannot see its internal synchronization and reports false races
inside it (seen in the OpenCV `GaussianBlur` smoke test). `called_from_lib:libtbb.so` ignores
only calls made from inside TBB. Nothing in FrameFlow code is suppressed. Cost: a real race
whose accesses happen inside TBB (e.g. freeing a `cv::Mat` while OpenCV still reads it on a
pool thread) would also be hidden, so ASan runs and clear frame ownership still matter.

**Deliberate-race check.** A temporary test had two `std::thread`s increment a plain `int`
captured by reference. The tsan preset reported `ThreadSanitizer: data race` with both stacks
at the `++counter` line, and `halt_on_error=1` made the test fail. After the test was removed
the suite passed again. This proved that races in our own code are still detected with
`tsan.supp` loaded, and that a race fails CI instead of only printing a warning. The test was
not kept.

**Limitation: TSan only sees executed paths.** It checks the happens-before order of accesses
that actually run, so it does not depend on lucky timing, but a race on a path no test
executes (e.g. shutdown while the queue is full, pop timeout) is invisible. Concurrency tests
must therefore exercise shutdown, timeout and overload paths, not just the happy path.
