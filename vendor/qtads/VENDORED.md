# Vendored: Qt Advanced Docking System

Upstream: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System
Commit:   edba03d0d122600ebaf0f6ba388c963d3a496b5d (2026-10-29, version 5.1.1)
License:  LGPL-2.1 (LICENSE, gnu-lgpl-v2.1.md)

Copied unmodified: CMakeLists.txt, adsConfig.cmake, cmake/, src/, licence files, except:
- CDockManager::registerDockWidget (DockManager.h/.cpp): makes a dock widget known without placing
  it, so the workbench builds only its saved layout at startup.
Left out: .git, doc/, examples/, demo/, sip/ and the Python packaging.
Built by src/frontend/qt_sdl/CMakeLists.txt with add_subdirectory; examples are off.
