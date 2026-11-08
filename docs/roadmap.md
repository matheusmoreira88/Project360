# Roadmap and verified status

The roadmap tracks evidence rather than estimated completion percentages.

| Phase | Status | Evidence / acceptance still needed |
|---|---|---|
| 0 — Foundation | Locally and remotely verified | CMake configure succeeded with CMake 4.3.1 and MSVC 19.51; Debug and Release builds completed; CTest passed in both configurations; smoke executable ran. Core Windows CI also passed on the Windows 2022 runner ([run 42451851612](https://github.com/matheusmoreira88/Project360/actions/runs/42451851612)). |
| 1 — PowerPC interpreter | Incremental subset implemented and locally tested; incomplete | Debug and Release builds and CTest passed after adding `mfspr`/`mtspr` for XER, LR and CTR, all eight CR bitwise logical operations, `mcrf`, `mcrxr`, `mfcr` and `mtcrf`, plus `rldcl`/`rldcr` variable rotate forms and the existing immediate and word rotate groups. CR/XER tests cover truth tables, bit and field preservation, source/target aliasing, special-register transfers, XER width and nibble clearing, and reserved encodings. Floating point, VMX128, exception delivery, atomics and most instructions remain absent. |
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

## Frontend status

The optional Qt 6 Widgets source now defines a dark Project360 shell, model-backed library, persistent local `.xex` paths, case-insensitive name search, name ordering, path removal, and explicit unavailable states. The independent library store has bounded, versioned parsing and passing unit tests. Windows CI compiled the GUI and passed the Qt model and offscreen window smoke tests in Debug and Release ([run 42451851613](https://github.com/matheusmoreira88/Project360/actions/runs/42451851613)). Manual visual review has not been performed, and core emulator phases remain independent of this optional target.

## Next engineering step

Continue with common integer operations and model CPU exception state, then broaden instruction coverage. The flat guest buffer still is not a virtual memory subsystem. Avoid starting loader or game-specific work until the CPU subset is broader and its semantics have a verified execution baseline.
