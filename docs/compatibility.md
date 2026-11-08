# Compatibility status

## Validation order

Compatibility work is prioritized from small controlled programs toward increasingly complete titles. GTA V is deferred until the first four gates have real, reproducible results; it is not the current development or testing focus. Every row below is a future target; none has been confirmed running yet.

| Order | Target class | What it should validate | Current status |
|---:|---|---|---|
| 1 | Small authorized homebrew | PowerPC execution, memory mapping, startup and implemented kernel calls | Not tested with external homebrew |
| 2 | Lightweight 2D game | 2D rendering path, controls, audio and basic title startup | Not tested |
| 3 | Simple 3D game | Shader execution, textures, draw submission and rendering | Not tested |
| 4 | More complex 3D game | Longer sessions, resource pressure, stability and measured performance | Not tested |
| 5 | Grand Theft Auto V, original Xbox 360 release | Long-term target, considered only after the four lighter gates have evidence | Deferred; not tested |

Move a target forward only with reproducible runs, logs and the exact input/build details. A successful parse or entry-point mapping does not count as a running program. For each gate, keep separate results for CPU/memory/kernel, graphics, controls/audio, and stability/performance so a pass in one area does not imply support in another.

The XEX2 parser recognizes metadata, including image flags, page descriptors, basic-compression blocks, import-library names, versions, and import-record addresses. The experimental loader decodes supported import-record fields and maps unencrypted XEX2 images using no compression or basic data/zero-fill compression with validated Xbox PowerPC PE32 sections. Present page descriptors restrict mapped permissions, and the entry-point page must be executable. Synthetic XEX fixtures exercise import thunks, but they do not establish external homebrew compatibility. An audit against the public [Xenia Xbox 360 export table](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc) corrected and regression-checked the pool import ordinals: `ExAllocatePool` 9, `ExAllocatePoolWithTag` 10 and `ExFreePool` 15. The time imports 130–132 and TLS imports 338–341 also agree with that table. Existing `Rtl*` and current-thread mappings still need audit; their previous ordinal claims are not considered verified Xbox 360 ABI support. The experimental pool uses guest page mappings, 16-byte alignment, adjacent-page coalescing for allocations larger than 4 KiB, exact-base frees and a configurable capacity; only nonpaged pool type 0 is modeled. The `--run-xex` path currently reserves at most 64 MiB for this pool, which is not complete Xbox kernel memory management. No commercial game or external homebrew application has been run. Other imports, encrypted images, LZX/other compression, kernel startup and general title loading remain unsupported. Compatibility with Xbox 360 titles, including the original Xbox 360 version of Grand Theft Auto V, is **not tested**.

The bounded `--run-xex` command can execute up to one million guest instructions from a successfully loaded supported unencrypted XEX2 image. It stops with an explicit fault diagnostic or an instruction-budget result; neither outcome implies game compatibility. No user-supplied XEX is currently available in the workspace for a run report.

The intended future reporting states are: not tested, unsupported, experimental, partial, and confirmed with a reproducible test. File recognition alone must never be reported as successful execution.
