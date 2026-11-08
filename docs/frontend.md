# Frontend status

The initial desktop frontend is a separate C++20/Qt Widgets executable called `project360_gui`. It uses an original dark theme, a navigation rail, a library view, and a details panel. The visual direction takes cues from desktop game launchers while using Project360 branding and no Steam assets.

## Current behavior

- Opens a native file picker filtered for `.xex` files.
- Saves selected local XEX paths in the user's application data directory and restores them on the next launch.
- Validates the `.xex` extension and that a selected path points to an existing regular file before adding it.
- Keeps entries whose source file was later moved or deleted, and labels those paths as missing.
- Shows the selected local path and a clear loader status.
- Hides the introductory banner after a file is added so the library and details have more room.
- Keeps **Iniciar** disabled because loading and execution are not implemented.
- Shows only the library navigation entry; other destinations will appear when they have working screens.
- Does not read or parse the executable contents.

The library file uses a small versioned format with bounded entry and path sizes and atomic replacement on Windows. It stores paths only. File names come from the local path and are not verified title metadata; the loader does not read or parse executable contents.

## Build

Install Qt 6.5 or newer with the Widgets module and make its CMake package files discoverable (for example with `CMAKE_PREFIX_PATH`). The GUI target is optional: CMake skips it when Qt is not found, while preserving the core executable and tests.

```powershell
cmake --preset windows-debug
cmake --build --preset debug --target project360_gui
```

This target has not yet been compiled or visually validated on the current development machine because a Qt SDK was not found. It should be built and checked on a Windows machine with Qt before the interface is treated as validated.

## Next frontend work

After the target is built and reviewed on Windows, move the widget list to a Qt model/view implementation and add search, sorting, and a real remove action. Keep unavailable actions visibly disabled until the corresponding backend is implemented.
