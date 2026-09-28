# Third-party notices

## cmake/FindAshitaSDK.cmake

Copyright (c) 2025 Ashita Development Team. Part of Ashita, licensed under the GNU General Public License, version 3
or (at your option) any later version - see the file's header and `cmake/COPYING` in the source repository (neither file
ships in the release archive). It is a build script used to compile this plugin; it is not part of the plugin DLL, and this
project's own licence (LICENSE) does not apply to it.

## Ashita v4 SDK and ImGui

TrueFont is compiled against the Ashita v4 plugin SDK (Ashita Development Team, GNU Lesser General Public License,
version 3 or later) and draws its settings panel with the ImGui that Ashita provides at runtime. Neither is copied into this repository or the release archive: the SDK is
checked out at build time, and ImGui is reached only through Ashita's interfaces.

## Nameplate

Aspect and Size find their three places in the game's code with byte patterns published by Nameplate
(https://github.com/Shirk/Nameplate, by Shirk) for interoperability. No Nameplate code is used.

## FreeType

Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights reserved.
TrueFont is based in part on the work of the FreeType Team.

TrueFont's second font engine (Settings > Quality > Engine: FreeType or Auto) is FreeType 2.14.3, built into the plugin
DLL from the source in `third_party/freetype/` (its modules for TrueType and CFF/OpenType outlines, their hinters, the
auto-hinter and the anti-aliasing renderer only). FreeType is dual-licensed; TrueFont uses it under the FreeType License
(`third_party/freetype/docs/FTL.TXT`; `third_party/freetype/LICENSE.TXT` explains the choice).

FreeType's hash functions (`src/base/fthash.c`, compiled into the DLL) carry their own notice:

> Copyright 2000 Computing Research Labs, New Mexico State University
> Copyright 2001-2015 Francesco Zappa Nardelli
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
> Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
> WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE COMPUTING
> RESEARCH LAB OR NEW MEXICO STATE UNIVERSITY BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
> OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

TrueFont builds FreeType without HarfBuzz, so the HarfBuzz bridge files in the auto-hinter's folder contribute no
code to the DLL (their contents are switched off), and FreeType's gzip (zlib), LZW, bzip2, Brotli, PNG and SVG
support is not built.

## Windows components and fonts

TrueFont draws its glyphs with Windows GDI, or with FreeType when Engine asks for it, and reads the game's own font image
with the Windows Imaging Component (WIC) when it has to. Both are part of Windows; nothing of them is copied into this repository or the release archive.

No font ships with TrueFont. It draws from the fonts already installed in your copy of Windows, in memory, while the
game runs, and never saves a font file (for FreeType it reads the chosen font's data from Windows into memory for the
length of a build).
