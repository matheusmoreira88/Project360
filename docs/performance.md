# Performance measurements

Performance claims must include the tested build, machine, workload, byte count, iteration count and method. Correctness tests are not performance measurements, and the emulator currently has no game frame-time or compatibility benchmark.

## Kernel memory comparison

`project360_kernel_memory_bench` measures full-match `RtlCompareMemory` calls over two equal guest-memory ranges. It warms up with three calls, verifies every result, then reports elapsed time and logical compared MiB per second. The implementation reads each range in fixed-size blocks; reported throughput counts one logical range, while the service reads two ranges.

Configure and run a reproducible Release build on Windows:

```powershell
cmake --preset windows-benchmarks
cmake --build --preset benchmarks-release
.\out\build\windows-benchmarks\Release\project360_kernel_memory_bench.exe 1048576 20
```

The optional arguments are bytes per comparison and iteration count. Defaults are 1 MiB and 20 iterations. To compare revisions, use the same machine, Release toolchain, arguments, power plan and background-load conditions; run each revision several times and report the distribution rather than a single best result. This benchmark does not measure XEX execution, GPU rendering or game performance.

### Initial measurement

On the development PC (AMD Ryzen 5 5500, 6 cores/12 hardware threads, Windows 10 Pro Education, MSVC 19.51), the Release benchmark compared two equal 1 MiB ranges for 20 iterations. Five consecutive runs reported a median logical throughput of **1,684.48 MiB/s**, with a range of **1,647.30–1,688.09 MiB/s** and median elapsed time of **11.8731 ms**. The compared-byte rate counts one range; each comparison reads two ranges. The run used the core implementation at `ce8bd46`. This is an initial microbenchmark sample with no controlled power-plan or background-load study, and it establishes no before/after speedup or game-performance claim.
