# Compatibility status

The XEX2 parser recognizes metadata, including import-library names, versions, and import-record addresses. The experimental loader decodes supported import-record fields and maps only unencrypted, uncompressed XEX2 images with validated Xbox PowerPC PE32 sections. A synthetic test program calls the implemented `RtlCompareMemory`, `RtlCompareMemoryUlong`, `RtlFillMemory`, `RtlFillMemoryUlong` and `RtlZeroMemory` kernel shims through guest import thunks and resumes its caller. No commercial game or external homebrew application has been run. Other imports, encrypted/compressed images, kernel startup and general title loading remain unsupported. Compatibility with Xbox 360 titles, including the original Xbox 360 version of Grand Theft Auto V, is **not tested**.

The intended future reporting states are: not tested, unsupported, experimental, partial, and confirmed with a reproducible test. File recognition alone must never be reported as successful execution.
