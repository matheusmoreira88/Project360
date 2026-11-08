# Xbox 360 kernel export map

The dispatcher accepts only the `xboxkrnl.exe` function exports listed below. Ordinals were checked against the pinned public Xenia export table at commit [`95a5c3e`](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc). The table is a symbol/ordinal reference; Project360 does not copy Xenia implementation code.

| Export | Hex ordinal | Decimal ordinal | Project360 behavior | Limits |
|---|---:|---:|---|---|
| `ExAllocatePool` | `0x009` | 9 | Implemented | Nonpaged pool type 0 only; bounded guest pool |
| `ExAllocatePoolWithTag` | `0x00A` | 10 | Implemented | Tag accepted but not retained; pool type 0 only |
| `ExFreePool` | `0x00F` | 15 | Implemented | Exact-base allocations only |
| `KeQueryInterruptTime` | `0x082` | 130 | Partial | Monotonic host uptime converted to 100 ns units |
| `KeQueryPerformanceFrequency` | `0x083` | 131 | Partial | Returns 49,875,000 guest counter ticks per second |
| `KeQuerySystemTime` | `0x084` | 132 | Partial | Host wall clock converted to 100 ns units since 1601 |
| `RtlCompareMemory` | `0x11A` | 282 | Partial | Returns matching prefix length; bounded reads |
| `RtlCompareMemoryUlong` | `0x11B` | 283 | Partial | Supports aligned ranges whose length is divisible by four |
| `RtlCompareString` | `0x11C` | 284 | Partial | Counted ANSI strings; ASCII-only case folding |
| `RtlCompareUnicodeString` | `0x11E` | 286 | Partial | Counted UTF-16BE; limited case-insensitive mapping |
| `RtlDowncaseUnicodeChar` | `0x124` | 292 | Partial | ASCII/Latin-1 plus U+0178; other codepoints rejected |
| `RtlFillMemoryUlong` | `0x126` | 294 | Implemented | Aligned destination and multiple-of-four length |
| `RtlInitAnsiString` | `0x12C` | 300 | Partial | Bounded NUL-terminated source scan |
| `RtlInitUnicodeString` | `0x12D` | 301 | Partial | Bounded NUL-terminated UTF-16BE source scan |
| `RtlNtStatusToDosError` | `0x135` | 309 | Partial | Tested subset of status mappings |
| `RtlUpcaseUnicodeChar` | `0x149` | 329 | Partial | ASCII/Latin-1 plus U+0178; other codepoints rejected |
| `KeTlsAlloc` | `0x152` | 338 | Partial | Main-thread TLS slot array only |
| `KeTlsFree` | `0x153` | 339 | Partial | Main-thread TLS slot array only |
| `KeTlsGetValue` | `0x154` | 340 | Partial | Main-thread TLS slot array only |
| `KeTlsSetValue` | `0x155` | 341 | Partial | Main-thread TLS slot array only |

All other ordinals return `unsupported_export`; imports from other libraries return `unsupported_library`. In particular, ordinal `0x068` (decimal 104) is `KeGetPRVRegister` in the reference table and is not implemented. It must not be treated as `KeGetCurrentThread`.

Several previously tested helper routines (`RtlCharToInteger`, `RtlFillMemory`, `RtlIntegerToUnicodeString`, `RtlEqualUnicodeString`, `RtlMoveMemory`, `RtlUlongByteSwap`, `RtlUshortByteSwap`, `RtlZeroMemory` and `RtlUnicodeStringToInteger`) are not exports in this table. Their old dispatcher routes have been removed. If a title imports such functionality from another module, that module and its own ordinal table require separate implementation.

This map establishes only the export names and ordinals for the pinned reference table. The listed services are not a complete Xbox 360 kernel, and no external homebrew or game has been run.
