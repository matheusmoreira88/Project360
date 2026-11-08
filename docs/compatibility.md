# Compatibility status

The XEX2 parser recognizes metadata, and the experimental loader can map only unencrypted, uncompressed XEX2 images with validated Xbox PowerPC PE32 sections. Automated tests load a synthetic fixture and execute one `addi` instruction from it. No commercial game or external homebrew application has been run. Encrypted/compressed images, imports, kernel startup and general title loading remain unsupported. Compatibility with Xbox 360 titles, including the original Xbox 360 version of Grand Theft Auto V, is **not tested**.

The intended future reporting states are: not tested, unsupported, experimental, partial, and confirmed with a reproducible test. File recognition alone must never be reported as successful execution.
