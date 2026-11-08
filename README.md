# Project360

Project360 is an original Xbox 360 emulation project. This repository contains an early foundation and a deliberately small PowerPC interpreter. A prototype loader can map only unencrypted, uncompressed XEX2 fixtures; general Xbox 360 executables and games are not yet supported.

## Current implementation

- C++20 project with CMake and Visual Studio x64 Debug/Release presets.
- A minimal executable that runs an embedded PowerPC smoke program.
- A 64-bit GPR CPU state and a bounded guest-memory buffer.
- Big-endian instruction fetch and bounded 8-, 16-, 32- and 64-bit guest data access. Multi-byte accesses enforce natural alignment. An opt-in 4 KiB page map translates guest virtual addresses to the bounded backing store and enforces read, write and execute permissions; checked byte and big-endian word-pattern fills validate the full mapped destination before changing bytes, including ranges crossing pages with non-contiguous physical backing. Flat identity access remains the default for existing CPU tests.
- Implemented instructions: `addi`, `addis`, `mulli`, `addic`/`addic.`, `subfic`, immediate `ori`/`oris`/`xori`/`xoris`/`andi.`/`andis.`, `add`, `addc`, `adde`, `addze`, `subf`, `subfc`, `subfe`, `mullw`/`mullwo`, `mulld`/`mulldo`, `extsb`, `extsh`, `extsw`, `cntlzw`, `cntlzd`, register logical operations (`and`, `andc`, `eqv`, `nand`, `nor`, `or`, `orc`, `xor`), `slw`, `srw`, `sraw`, `srawi`, `sld`, `srd`, `srad`, `rlwinm`, `cmp`/`cmpl`, `cmpi`/`cmpli`, D-form and indexed loads/stores for byte, halfword, word and doubleword widths, `lmw`/`stmw`, indexed byte-reversed `lhbrx`, `lwbrx`, `sthbrx`, and `stwbrx`, `b`/`ba`, `bc`/`bca` with supported BO forms, `bclr`, and `bcctr` with supported BO/BH forms. Doubleword support includes `ld`/`ldu`, `std`/`stdu`, `ldx`/`ldux`, and `stdx`/`stdux`; the unsupported `lwa` form is diagnosed. `cntlzw` counts leading zeros in the low 32 bits; `cntlzd` examines all 64 bits. `srawi` uses an immediate 0–31 bit shift, sign-extends the low-word result and updates XER.CA for discarded one bits. `sld`/`srd` zero the result for shift amounts 64–127; `srad` also models arithmetic fill and XER.CA. D-form and indexed update accesses reject invalid base/destination register combinations and update RA only after successful memory access. Arithmetic XO forms support OE and Rc where defined; carry instructions track XER.CA with carry-out/no-borrow semantics.
- CR operations include `mfcr`, `mtcrf`, `mcrf`, `mcrxr` and the eight bitwise forms; `mfspr`/`mtspr` support XER, LR, CTR, SRR0 and SRR1, with SRR access privileged by the modeled MSR.PR bit. Illegal and privileged instructions enter vector `0x700`; `sc 0` enters the system-call vector `0xC00`, saves the following PC and can return through `rfid`. Hypervisor `sc` level 1 is explicitly unsupported.
- Bounded XEX2 parsing reads module flags, security-block size, image size, Title ID, Media ID, entry point, file-format flags, and import-library names, versions, and record addresses. The loader decodes function/data type, library index and ordinal from PE import records before mapping the image. It accepts only unencrypted, uncompressed XEX2 files, validates Xbox PowerPC big-endian PE32 sections, and maps them with page permissions; encrypted and compressed images are explicitly rejected without decryption.
- A guest execution wrapper intercepts decoded function-import thunks by guest PC. It currently implements `xboxkrnl.exe` ordinals 268 (`RtlCompareMemory`), 284 (`RtlFillMemory`), 285 (`RtlFillMemoryUlong`) and 320 (`RtlZeroMemory`) with Xenon argument registers and LR return semantics. Compare returns the byte count up to the first mismatch in GPR3; `RtlFillMemoryUlong` validates 4-byte alignment and repeats its pattern in guest big-endian order. A synthetic XEX test calls all four shims from PowerPC code and resumes its caller. Other kernel exports, data-import resolution, thunk rewriting and real title startup remain unsupported.
- `project360 --inspect-xex <path>` prints metadata from a supported XEX2 file while explicitly reporting that the image has not been loaded or executed.
- Unsupported or invalid instruction forms report explicit faults. Arithmetic `record` on `add` updates CR0.
- Dependency-free test executables covering PowerPC behavior, guest page mapping, and persistent library storage, including corrupt input handling.

This is not yet a complete PowerPC or Xenon implementation. Guest exception delivery covers illegal/privileged program exceptions and the base `sc 0` path only; instruction/data alignment and access failures remain host-side diagnostics, and other exception classes, Xenon-specific MSR behavior, floating-point arithmetic and FPSCR control, VMX128, atomics, Xbox 360 MMU behavior, broad kernel service dispatch, general XEX image loading, GPU, audio and input remain unsupported. Scalar FPR memory transfers (`lfs`, `lfd`, `stfs`, `stfd`) and bitwise move/sign operations (`fmr`, `fneg`, `fabs`, `fnabs`) have initial D/indexed support where applicable, but the floating-point unit remains incomplete. The page map is a generic prototype, not a reproduction of Xenon translation or its 512 MB unified memory. The desktop frontend is an early scaffold described below. The instruction and data access behavior here is a testable initial subset, not a compatibility claim for commercial titles.

## Desktop interface (early scaffold)

A separate Qt 6 Widgets desktop target provides a Steam-inspired library shell with a local `.xex` file picker, persistent paths, search, name sorting, and removal from the library. It never deletes the original file, parses executable metadata, or runs the XEX. The **Iniciar** action is intentionally disabled until the loader and required runtime subsystems exist. Qt 6.5 or newer with the Widgets module is needed to build this optional target. If Qt is not found, CMake still configures the core, smoke executable, and tests.

## Build and run on Windows

Install CMake 3.24 or newer and Visual Studio 2026 with the x64 C++ toolchain, then from this directory run:

```powershell
cmake --preset windows-debug
cmake --build --preset debug
ctest --preset debug
.
out\build\windows-debug\Debug\project360.exe
```

The smoke program should report `r3=7 r4=12 r5=19`. The test target uses a small built-in harness and requires no downloaded test framework. Toolchain availability and these commands must be verified on the target machine before claiming a successful build.

When Qt is installed and discoverable by CMake, the GUI target is `project360_gui`:

```powershell
cmake --build --preset debug --target project360_gui
```

See [docs/frontend.md](docs/frontend.md) for the current interface scope and build note.

## Project status

See [docs/roadmap.md](docs/roadmap.md), [docs/testing.md](docs/testing.md) and [docs/compatibility.md](docs/compatibility.md). No commercial game compatibility or performance result has been measured.
