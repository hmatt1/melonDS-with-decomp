# Vendored: Tracy profiler (client only)

Upstream: https://github.com/wolfpld/tracy
Commit:   f2a4f2529e7e71844c9e434b877770a21ecf7587 (2026-11-05, version 0.15.0)
License:  BSD-3-Clause (LICENSE)

Copied unmodified: public/ (the client: TracyClient.cpp and the headers), LICENSE.
Left out: the viewer (profiler/), capture/, the other tools, docs, examples, and the
Fortran binding (public/TracyClient.F90).
Built by src/frontend/qt_sdl/CMakeLists.txt when MELONDS_TRACE is set; see
src/frontend/qt_sdl/Trace.h. The viewer must be the same version (0.15.0).
