# Roadmap and verified status

The roadmap tracks evidence rather than estimated completion percentages.

| Phase | Status | Evidence / acceptance still needed |
|---|---|---|
| 0 — Foundation | Locally and remotely verified | CMake configure succeeded with CMake 4.3.1 and MSVC 19.51; Debug and Release builds completed; CTest passed in both configurations; smoke executable ran. Core Windows CI also passed on the Windows 2022 runner ([run 42451851612](https://github.com/matheusmoreira88/Project360/actions/runs/42451851612)). |
| 1 — PowerPC interpreter | Incremental subset implemented and locally tested; incomplete | Debug and Release builds and CTest pass. Tests cover illegal/privileged program exception entry to vector `0x700`, `sc 0` system-call entry to `0xC00` and return through `rfid`, SRR0/SRR1 capture, privileged SRR access, scalar FPR memory transfers in D/indexed forms, and bit-exact `fmr`, `fneg`, `fabs`, `fnabs` behavior. Kernel syscall dispatch, hypervisor calls, general floating-point arithmetic/FPSCR control, other exception classes, Xenon-specific MSR behavior, VMX128, atomics and most instructions remain absent. |
| 2 — Memory subsystem | Mapping prototype implemented; incomplete | Opt-in 4 KiB guest-page mapping, virtual aliases, read/write/execute permissions, bounded physical backing, CPU execute-permission checks, and transactional fill across non-contiguous mapped pages are covered by tests. Xenon-specific translation, MMIO, synchronization, CPU/GPU coherence and memory budgeting remain unimplemented. |
| 3 — Loader | Unencrypted XEX2 homebrew loading started; incomplete | Bounded metadata parsing, PE32 validation and synthetic malformed-input tests cover core headers, security bounds, execution metadata, PE sections and ranges. The loader maps only unencrypted/uncompressed XEX2 payloads, and the interpreter executes a synthetic PPC instruction from the loaded image. CLI metadata inspection is available. Encrypted/compressed formats, imports, real title startup and kernel integration remain unsupported. |
| 4 — Kernel | First export shim implemented; incomplete | A direct dispatcher recognizes `xboxkrnl.exe` ordinal 320 (`RtlZeroMemory`), validates the 32-bit guest arguments, clears virtual memory transactionally, and returns through LR. Unit tests cover dispatch, permissions, failure atomicity, unknown imports and CPU-state preservation. XEX import parsing/thunk patching, exception-to-service routing, support-state inventory and the controlled PPC program remain unimplemented; this does not yet run XEX imports. |
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

Integrate the unencrypted XEX2 loader with the frontend and build a controlled test executable with a minimal kernel call path. Encrypted/compressed commercial XEX files remain unsupported, and real title execution also requires broader CPU semantics, imports and kernel services.
