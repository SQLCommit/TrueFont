# Building

[Back to TrueFont](README.md)

For normal installation, download the plugin ZIP from the release page. These steps are for building the source.

## Requirements

- Windows with Visual Studio 2022 and its C++ tools.
- CMake 3.22 or newer.
- The Ashita v4 SDK folder containing `Ashita.h`.

## Build

From the project folder in Command Prompt:

```bat
set ASHITA4_SDK_PATH=C:\path\to\ashita-sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

CMake builds for **32-bit x86** and writes `build\Release\truefont.dll`.

The DLL includes FreeType 2.14.3 (Settings > Quality > Engine), built from `third_party/freetype/`: only the files the
build uses, byte for byte as in the release tarball (`third_party/freetype/VENDORED.txt` lists them with the tarball's
SHA-256), with TrueFont's module list and options in `third_party/freetype-config/`. Nothing is downloaded while
building.

Fully close FFXI before replacing `/ashita/plugins/truefont.dll`, then relaunch and `/load truefont`.

## Release documentation

Edit the root `README.md` for GitHub. Both release workflows generate a plain Markdown `docs/truefont/README.md`
inside the ZIP: badges become links, image headings become text, and dropdowns are expanded. The source README stays
unchanged.

To preview the packaged README in PowerShell:

```powershell
./.github/scripts/export-readme.ps1 -Output build/README.release.md
```

The README is read from the revision being packaged. Documentation edits need to be included in that revision before
preparing its release files; existing ZIPs do not update automatically.
