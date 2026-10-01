# Vendored copy of libjpeg-turbo 3.2.0

- Source: https://github.com/libjpeg-turbo/libjpeg-turbo/releases/latest (v3.2.0)
- Trimmed: simd/ (built with WITH_SIMD=OFF, no NASM needed), doc/, testimages/,
  jna/, fuzz/, .github/
- Patched: CMakeLists.txt replaces CMAKE_SOURCE_DIR with CMAKE_CURRENT_SOURCE_DIR
  so it can be integrated via add_subdirectory() (upstream forbids subproject
  use; this is a local vendoring patch). Note this also disables the upstream
  add_subdirectory() guard.
- Its own ctest suite is not usable from this trimmed copy (testimages removed);
  img2png runs its own tests instead.
