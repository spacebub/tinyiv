# Compiling tinyiv

Needs a C++23 compiler, CMake 3.25, SDL3, libvips with its C++ API, libjpeg, libpng, libwebp,
libjxl, libtiff, libheif, zlib and zstd. The benchmarks in `bench/` build on Linux only, see [bench/README.md](bench/README.md).

## Linux

Install the libraries and pkg-config from the distribution, then:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/bin/tinyiv photo.jpg
```

Options:

```
-DTIV_RELEASE=ON       no git revision beside the version
-DTIV_BENCHMARKS=ON    the benchmark suite in bench/
-DTIV_SANITIZE=ON      address and undefined sanitizers
```

`cmake --install build --prefix ~/.local` puts the binary in `bin`, and the desktop entry and
the icons under `share`.

## Windows

The libraries come from [vcpkg](https://github.com/microsoft/vcpkg), which CMake drives
through the manifest in `vcpkg.json`. Nothing is installed by hand: the first configure builds
them all, and every later one restores them from vcpkg's cache.

### Tools

- Visual Studio 2026 with the *Desktop development with C++* workload, for MSVC.
- CMake 3.25 or later on the path, from `winget install --id Kitware.CMake`. Visual Studio
  brings its own CMake and Ninja too, on the path only in the *Developer PowerShell for VS
  2026*.
- Git.
- [Inno Setup](https://jrsoftware.org/isinfo.php) 6 or 7, only to build the setup:
  `winget install --id JRSoftware.InnoSetup`.

### vcpkg

Clone and bootstrap it once, anywhere, then point `VCPKG_ROOT` at it for good. In PowerShell:

```powershell
git clone https://github.com/microsoft/vcpkg G:\Projects\vcpkg
G:\Projects\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "G:\Projects\vcpkg", "User")
```

Open a new terminal so it sees `VCPKG_ROOT`. The versions are pinned by `builtin-baseline` in
`vcpkg.json`, a commit of the vcpkg repository, so a clone older than it needs a
`git -C $env:VCPKG_ROOT pull`.

What the repository adds to vcpkg, both picked up through `vcpkg-configuration.json`:

- `vcpkg-triplets/x64-windows-release.cmake`, the default triplet on Windows. It builds DLLs
  as `x64-windows` does, but only their release half, which halves the first build.
- `vcpkg-ports/libvips`, the upstream port with features for the libvips options it has none
  for. The upstream portfile turns every such option off, which loses the built-in GIF, PPM,
  Radiance and Analyze loaders, and PDF, JPEG XL, HEIF and AVIF, OpenEXR, JPEG 2000 and
  highway. `vcpkg.json` lists libheif itself only to leave out its x265 encoder, which vcpkg
  builds by default otherwise.

### Build

In any PowerShell, from the repository:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 --toolchain "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
build\bin\Release\tinyiv.exe photo.jpg
```

The first configure runs `vcpkg install` for the manifest, into `build\vcpkg_installed`, and
takes the better part of an hour: about 55 packages, one at a time, many of them through
autotools. The packages are kept as zips in `%LOCALAPPDATA%\vcpkg\archives`, so a new build
tree, or this one after a clean, restores them in seconds. They build again only for a new
baseline, other features or triplet, or a Visual Studio update, as the compiler is part of
each package's hash. vcpkg copies the DLLs beside `tinyiv.exe` as it builds.

`Debug` builds as well. With the release only triplet every configuration uses the release C
runtime, since the C++ API of libvips passes standard containers across the DLL boundary.

The build type is fixed at configure time with Ninja instead. Open the *Developer PowerShell
for VS 2026*, where MSVC is on the path, and:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release --toolchain "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build
build\bin\tinyiv.exe photo.jpg
```

To use the full `x64-windows` triplet, with the debug libraries, pass
`-DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_HOST_TRIPLET=x64-windows` on the first configure.
It works only with Ninja and a build type: pkg-config resolves one configuration, and the
configure stops with an error otherwise. A build tree keeps the triplet it was first
configured with.

### Install

```powershell
cmake --install build --config Release --prefix C:\Tools\tinyiv
```

Puts `tinyiv.exe` in `bin` with the DLLs it loads and the Visual C++ runtime, and fontconfig's
configuration in `etc\fonts`, which librsvg loads. The folder runs on any Windows 10 or 11 as
it is.

### Setup

With Inno Setup installed when the build tree was configured:

```powershell
cmake --build build --config Release --target installer
```

Stages a fresh install in `build\stage` and compiles `build\tinyiv-<version>-setup.exe` from
it, through the script CMake configures from `packaging/windows/tinyiv.iss.in`. The setup
installs for the current user into `%LOCALAPPDATA%\Programs\tinyiv` and asks for no
elevation. Its *associate* task puts tinyiv in the *Open with* list of every image type
libvips reads and under *Settings > Apps > Default apps*. Windows lets no setup make itself
the default, so that is chosen there. The uninstaller takes it all out again.

The `AppId` GUID in the script is tinyiv's identity to Windows, and stays as it is: a setup
with another replaces nothing and installs a second copy.

### Known differences from Linux

- vcpkg has librsvg 2.40, the last C version. Its filters are much slower than the Rust
  librsvg distributions ship, so an SVG with blurs takes seconds to render at a large zoom.
- AVIF decodes through aom, as the vcpkg libheif turns dav1d off. It is slower than the dav1d
  most distributions build libheif with.
