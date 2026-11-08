# Roadmap and verified status

The roadmap tracks evidence rather than estimated completion percentages.

| Phase | Status | Evidence / acceptance still needed |
|---|---|---|
| 0 — Foundation | Locally verified | CMake configure succeeded with CMake 4.3.1 and MSVC 19.51; Debug and Release builds completed; CTest passed in both configurations; smoke executable ran. Windows CI workflow is configured but has not run remotely. |
| 1 — PowerPC interpreter | Incremental subset implemented and locally tested; incomplete | Debug and Release builds and CTest passed after adding 64-bit big-endian accesses and `ld`/`ldu`/`std`/`stdu` plus indexed forms. Existing arithmetic, branch, word-memory and byte-reversed subsets remain tested. Floating point, VMX128, exceptions, atomics and most instructions remain absent. |
| 2 — Memory subsystem | Flat-buffer widths extended; virtual memory not started | 8/16/32/64-bit big-endian accesses are bounded; multi-byte accesses enforce natural alignment. Guest address mapping, protection, translation and synchronization remain unimplemented. |
| 3 — Loader | Not started | Safe format parsing and authorized test executable. |
| 4 — Kernel | Not started | Explicit service support states and controlled test program. |
| 5 — GPU | Not started | Xenos command tests and Vulkan rendering evidence. |
| 6 — Audio, input and storage | Not started | Device integration tests. |
| 7 — IR/JIT | Not started | Differential correctness and measured speedup. |
| 8 — Integration | Not started | Stable multi-subsystem program tests. |
| 9 — GTA V / other games | Not started | No game loading or execution is currently supported. |
| 10 — Performance | Not started | Reproducible benchmarks and profiling. |
| 11 — Expansion | Not started | Depends on prior compatibility evidence. |

## Next engineering step

Continue with additional integer operations and branch forms, then model CPU exception state and broaden instruction coverage. The flat guest buffer still is not a virtual memory subsystem. Avoid starting loader or game-specific work until the CPU subset is broader and its semantics have a verified execution baseline.
