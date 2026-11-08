# Roadmap and verified status

The roadmap tracks evidence rather than estimated completion percentages.

| Phase | Status | Evidence / acceptance still needed |
|---|---|---|
| 0 — Foundation | Locally and remotely verified | CMake configure succeeded with CMake 4.3.1 and MSVC 19.51; Debug and Release builds completed; CTest passed in both configurations; smoke executable ran. Core Windows CI also passed on the Windows 2022 runner ([run 42451851612](https://github.com/matheusmoreira88/Project360/actions/runs/42451851612)). |
| 1 — PowerPC interpreter | Incremental subset implemented and locally tested; incomplete | Debug and Release builds and CTest pass. Tests now cover illegal/privileged program exception entry to vector `0x700`, SRR0/SRR1 capture, privileged SRR access, and `rfid` return. Other exception classes, Xenon-specific MSR behavior, floating point, VMX128, atomics and most instructions remain absent. |
| 2 — Memory subsystem | Mapping prototype implemented; incomplete | Opt-in 4 KiB guest-page mapping, virtual aliases, read/write/execute permissions, bounded physical backing and CPU execute-permission checks are covered by tests. Xenon-specific translation, MMIO, synchronization, CPU/GPU coherence and memory budgeting remain unimplemented. |
| 3 — Loader | XEX2 metadata parsing started; incomplete | Bounded in-memory and file-header parsers plus synthetic malformed-input tests cover core header, security-block bounds, execution metadata and entry point. Decryption, decompression, PE parsing, section mapping, imports and executable startup are not implemented. |
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

Expand the XEX2 parser into safe file reading and PE metadata parsing, then design segment loading against the generic page map. The current parser only reads metadata; title execution remains blocked on encrypted/compressed image handling, broader CPU semantics and kernel services.
