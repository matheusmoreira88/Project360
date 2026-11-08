# Roadmap and verified status

The roadmap tracks evidence rather than estimated completion percentages.

| Phase | Status | Evidence / acceptance still needed |
|---|---|---|
| 0 — Foundation | Locally verified | CMake configure succeeded with CMake 4.3.1 and MSVC 19.51; Debug and Release builds completed; CTest passed in both configurations; smoke executable ran. Windows CI workflow is configured but has not run remotely. |
| 1 — PowerPC interpreter | Incremental subset implemented and locally tested; incomplete | Implemented/tested integer immediates, add/subtract carry families, low-word and low-doubleword multiplication, byte/halfword/word sign extension, 64-bit register logical operations, word shifts/rotate-mask, signed/unsigned register and immediate compares, byte/halfword/word D-form and indexed data access with update, byte-reversed and multiple-word forms, relative/absolute immediate branches and register-target conditional branches. Floating point, VMX128, exceptions, atomics and most instructions remain absent. |
| 2 — Memory subsystem | Not started | Guest address mapping, protection, broader widths, translation and synchronization. |
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

Continue with additional integer operations and branch forms, then implement exception semantics and broader instruction coverage. Avoid starting loader or game-specific work until the CPU subset is broader and its semantics have a verified execution baseline.
