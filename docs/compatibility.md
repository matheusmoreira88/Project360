# Compatibility status

## Validation order

Compatibility work is prioritized from small controlled programs toward increasingly complete titles. Every row below is a future test target; none has been confirmed running yet.

| Order | Target class | What it should validate | Current status |
|---:|---|---|---|
| 1 | Small authorized homebrew | PowerPC execution, memory mapping, startup and implemented kernel calls | Not tested with external homebrew |
| 2 | Lightweight 2D game | 2D rendering path, controls, audio and basic title startup | Not tested |
| 3 | Simple 3D game | Shader execution, textures, draw submission and rendering | Not tested |
| 4 | More complex 3D game | Longer sessions, resource pressure, stability and measured performance | Not tested |
| 5 | Grand Theft Auto V, original Xbox 360 release | Apply the same general compatibility gates after the earlier classes; it remains the long-term principal goal | Not tested |

Move a target forward only with reproducible runs, logs and the exact input/build details. A successful parse or entry-point mapping does not count as a running program.

The XEX2 parser recognizes metadata, including image flags, page descriptors, basic-compression blocks, import-library names, versions, and import-record addresses. The experimental loader decodes supported import-record fields and maps unencrypted XEX2 images using no compression or basic data/zero-fill compression with validated Xbox PowerPC PE32 sections. Present page descriptors restrict mapped permissions, and the entry-point page must be executable. A synthetic XEX fixture contains thirteen kernel imports and calls `RtlCompareString` through its thunk in both case-sensitive and ASCII case-fold modes, alongside twelve other imported routines. `RtlCharToInteger` and `RtlNtStatusToDosError` currently have direct-dispatch tests only. `RtlUpcaseUnicodeChar` and `RtlDowncaseUnicodeChar` are dispatched at their public Xbox kernel ordinals and implement stable ASCII/Latin-1 simple-case mappings plus U+0178; other Unicode codepoints are explicitly unsupported. `RtlCompareUnicodeString` and `RtlEqualUnicodeString` validate counted UTF-16BE descriptors and compare case-sensitively across UTF-16 code units; case-insensitive comparison uses the same limited case table and diagnoses unsupported characters. The ordinal names are cross-checked against the public [XboxDev export list](https://github.com/XboxDev/xbedump/blob/master/xboxkrnl.h). The experimental kernel pool implements the nonpaged `ExAllocatePool`/`ExAllocatePoolWithTag` and `ExFreePool` ordinals with guest page mappings, 16-byte alignment, adjacent-page coalescing for allocations larger than 4 KiB, exact-base frees and a configurable capacity; only pool type 0 is supported. A synthetic loaded-XEX integration test calls allocate and free through import thunks and writes to the returned guest address. The `--run-xex` path currently reserves at most 64 MiB for this lazy guest pool; this is an early pool model, not complete Xbox kernel memory management. `RtlCompareString` rejects non-ASCII case-fold requests explicitly until an Xbox-compatible ANSI upcase table is available. No commercial game or external homebrew application has been run. Other imports, encrypted images, LZX/other compression, kernel startup and general title loading remain unsupported. Compatibility with Xbox 360 titles, including the original Xbox 360 version of Grand Theft Auto V, is **not tested**.

The bounded `--run-xex` command can execute up to one million guest instructions from a successfully loaded supported unencrypted XEX2 image. It stops with an explicit fault diagnostic or an instruction-budget result; neither outcome implies game compatibility. No user-supplied XEX is currently available in the workspace for a run report.

The intended future reporting states are: not tested, unsupported, experimental, partial, and confirmed with a reproducible test. File recognition alone must never be reported as successful execution.
