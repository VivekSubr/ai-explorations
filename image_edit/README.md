# SVG Playground

A native Qt 6 desktop playground for editing and previewing SVG markup.

## Features

- Live SVG preview with validation
- Built-in SVG presets
- Zoom, fit-to-view, and canvas panning
- Light, dark, transparent, and checkerboard backgrounds
- Open and save SVG files
- Copy SVG markup and export transparent PNG images
- Unsaved-change protection and persisted window layout

## Requirements

- CMake 3.21 or newer
- Qt 6.5 or newer with the Widgets and SVG modules
- A C++20 compiler

## Build

Configure Qt's environment first, then run:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

With the standard Windows Qt installation, you can provide its CMake package directly:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\msvc2022_64"
cmake --build build --config Release
```

The executable is named `svg-playground`.

## Controls

- Use the mouse wheel or toolbar buttons to zoom.
- Drag with the middle mouse button, or Space + left mouse button, to pan.
- Use **Ctrl+0** for actual size and **Ctrl+9** to fit the SVG to the canvas.
