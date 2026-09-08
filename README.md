# AI Stroke Painter

AI Stroke Painter is an AI-first illustration application built as a native
C++ fork. It is not a Krita plugin and does not include a PyKrita runtime.

The focused workspace turns a text prompt into an editable raster layer on the
current canvas. It has two generation paths:

- A deterministic local concept-sketch renderer for offline composition work.
- An OpenAI-compatible image-generation API path that imports the returned
  image directly as a native layer.

The API key is held only for the active request; endpoint and model choices
are stored locally, but credentials are never persisted. Remote endpoints must
use HTTPS unless they target a loopback development server.

## What is included

- AI prompt workspace, canvas creation, preview, cancellation, and native
  layer insertion.
- Native `.kra` document read/write support and PNG/Qt image import/export.
- Colour management required by the retained document/canvas foundation.
- A deliberately small application menu: document operations, undo/redo, and
  the AI workspace.

The upstream Python plugins, SIP bindings, general paint-engine catalogue,
format plugins, templates, bundled workspaces, and Krita-specific installers
are excluded from this fork's build and runtime package.

## Build with Craft on Windows

After preparing `C:\CraftRoot`, configure and build from this directory:

```powershell
$env:PATH = 'C:\CraftRoot\dev-utils\meson-venv\Scripts;C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;' + $env:PATH
$env:PKG_CONFIG_PATH = 'C:\CraftRoot\lib\pkgconfig'

cmake -S . -B build-ai -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_QT6=ON
cmake --build build-ai --target all -- -j4
cmake --install build-ai --prefix C:\CraftRoot\ai-stroke-painter
```

The install prefix is intentionally outside the source directory so generated
objects and distributable files do not clutter the project tree.

To launch the Craft-built application:

```powershell
$packageBin = 'C:\CraftRoot\ai-stroke-painter\bin'
$env:PATH = "$packageBin;C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;" + $env:PATH
$env:QT_PLUGIN_PATH = 'C:\CraftRoot\plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'C:\CraftRoot\plugins\platforms'
& "$packageBin\ai-stroke-painter.exe"
```

## Licensing and provenance

This repository retains components derived from Krita. Their original
copyright notices and licences remain in place; the project is distributed
under GPL-2.0-or-later unless an individual file states otherwise.
