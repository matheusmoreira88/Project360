# Testing

The dependency-free CTest suite contains `project360_tests` and `project360_library_tests`. When Qt 6 is available, CMake also adds `project360_qt_model_tests`. The CPU test checks small guest programs in memory, including immediate/register arithmetic and multiply (including signed boundaries, low-word/full-product behavior and overflow), byte/halfword/word sign extension, `cntlzw`/`cntlzd` zero and leading-bit cases, immediate/register logical operations, `slw`/`srw`/`sraw`/`srawi`/`sld`/`srd`/`srad` (including immediate and register counts, XER.CA and Rc), `rlwinm` including wrapping masks, signed and unsigned register/immediate compares at 32 and 64 bits, OE/Rc effects, XER carry/overflow behavior, byte/halfword/word/doubleword D-form and indexed loads/stores including update, signed DS displacement, RA=0 D-form addressing, byte-reversed and multiple-word forms, RA=0 indexed addressing, base-register commit on success only, sign extension, invalid update operands, unsupported `lwa` and reserved encodings, alignment and bounds faults, all-or-nothing register results for failed `lmw`, prevalidated stores for failed `stmw`, relative/absolute `b` and `bc`, `bclr`, `bcctr`, CR field preservation, big-endian data and unsupported/reserved instruction faults.

The library-store test covers missing-library startup, adding and reloading absolute UTF-8 paths, duplicate rejection, extension and missing-file validation, removal persistence, and truncated-file rejection without exposing partial entries. The conditional Qt model test covers case-insensitive search, ascending/descending title order, duplicate rejection, model removal, persistence after removal, keeping the original XEX file intact, and deferred file-existence checks for restored library paths.

From a Windows Developer PowerShell with CMake 3.24+ and Visual Studio 2026 C++ tools installed:

```powershell
cmake --preset windows-debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --build --preset release
ctest --test-dir out/build/windows-debug -C Release --output-on-failure
```

Latest local validation on Windows/MSVC: Debug and Release builds succeeded, both available CTest cases passed in each configuration, and the Debug smoke executable printed `r3=7 r4=12 r5=19`. The optional Qt GUI and its model test are not part of these passing builds because Qt 6 is not installed on this PC.

These tests are not evidence for instructions outside the listed subset, timing behavior, multi-threaded execution, or Xbox 360 game compatibility. Add one test per implemented instruction and targeted boundary tests as semantics expand.
