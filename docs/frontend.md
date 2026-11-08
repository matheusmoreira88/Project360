# Frontend status

The initial desktop frontend is a separate C++20/Qt Widgets executable called `project360_gui`. It uses an original dark theme, a navigation rail, a library view, and a details panel. The visual direction takes cues from desktop game launchers while using Project360 branding and no Steam assets.

## Current behavior

- Opens a native file picker filtered for `.xex` files.
- Lists selected existing files in memory for the current application session.
- Shows the selected local path and a clear loader status.
- Keeps **Iniciar** disabled because loading and execution are not implemented.
- Leaves Recentes and Configurações disabled until those functions have backends.
- Does not read or parse the executable contents.

The selection is not saved across restarts. File names come from the local path and are not verified title metadata.

## Build

Install Qt 6.5 or newer with the Widgets module and make its CMake package files discoverable (for example with `CMAKE_PREFIX_PATH`). The GUI target is optional: CMake skips it when Qt is not found, while preserving the core executable and tests.

```powershell
cmake --preset windows-debug
cmake --build --preset debug --target project360_gui
```

This target has not yet been compiled or visually validated on the current development machine because a Qt SDK was not found. It should be built and checked on a Windows machine with Qt before the interface is treated as validated.

## Next frontend work

After the target is built and reviewed on Windows, add persistent library metadata only alongside a safe library model, then wire settings and recent execution to real emulator subsystems as those become available. Keep unavailable actions visibly disabled until the corresponding backend is implemented.
