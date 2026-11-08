# Testing

The initial test suite is the dependency-free `project360_tests` executable registered with CTest. It checks small guest programs in memory, including immediate/register arithmetic and multiply (including signed boundaries, low-word/full-product behavior and overflow), byte/halfword/word sign extension, immediate/register logical operations, `slw`/`srw`/`sraw`, `rlwinm` including wrapping masks, signed and unsigned register/immediate compares at 32 and 64 bits, OE/Rc effects, XER carry/overflow behavior, byte/halfword/word D-form and indexed loads/stores including update, byte-reversed and multiple-word forms, RA=0 indexed addressing, base-register commit on success only, sign extension, invalid update operands, reserved and architecturally undefined encodings, alignment and bounds faults, all-or-nothing register results for failed `lmw`, prevalidated stores for failed `stmw`, relative/absolute `b` and `bc`, `bclr`, `bcctr`, CR field preservation, big-endian data and unsupported/reserved instruction faults.

From a Windows Developer PowerShell with CMake 3.24+ and Visual Studio 2026 C++ tools installed:

```powershell
cmake --preset windows-debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

These tests are not evidence for instructions outside the listed subset, timing behavior, multi-threaded execution, or Xbox 360 game compatibility. Add one test per implemented instruction and targeted boundary tests as semantics expand.
