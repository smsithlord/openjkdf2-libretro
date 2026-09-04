# Third-party notices

What the libretro core (`openjkdf2_libretro.dll`) is built from, beyond
OpenJKDF2 itself. OpenJKDF2's own terms are in [LICENSE.md](LICENSE.md):
"Jedi Knight: Dark Forces II" and the game data are Lucasfilm's; the
reimplementation is by the OpenJKDF2 contributors under the permissive grant
stated there. No game assets are distributed with the core.

Full license texts ship with each component's source in this repository at
the paths given.

## Linked into the core

| Component | License | Source in tree |
|---|---|---|
| SDL 3 | zlib | `lib/SDL/LICENSE.txt` |
| SDL3_mixer | zlib | `lib/SDL_mixer/LICENSE.txt` |
| libogg, libvorbis, opus, opusfile (via SDL3_mixer) | BSD-3-Clause | `lib/SDL_mixer/external/*/COPYING` |
| GLEW | Modified BSD / MIT | `lib/glew/LICENSE.txt` |
| freeglut | MIT (X-Consortium style) | `lib/freeglut/COPYING` |
| zlib | zlib | `lib/zlib/LICENSE` |
| libpng | PNG Reference Library License | `lib/libpng/LICENSE` |
| nlohmann/json | MIT | `3rdparty/json/LICENSE.MIT` |
| libsmacker (Greg Kennedy) | LGPL-2.1-or-later | `src/external/libsmacker/smacker.h` |
| libsmusher (Max Thomas) | MIT | `src/external/libsmusher/LICENSE` |
| fcaseopen (Keith Bauer) | MIT | `src/external/fcaseopen/` |
| nativefiledialog-extended | zlib | `src/external/nativefiledialog-extended/` |
| Berkeley Yacc (COG parser generator) | Public domain | `byacc/README` |
| flex (COG lexer generator) | BSD (Regents of the University of California) | `flex/COPYING` |

libsmacker is LGPL and is compiled into the core. The LGPL's relinking
requirement is met by this being open source: the complete corresponding
source of the core, including libsmacker unmodified, is this repository.

## Shipped beside the core

| Component | License | Note |
|---|---|---|
| OpenAL Soft (`OpenAL32.dll`) | LGPL-2.1 (portions BSD-3-Clause) | `lib/openal/COPYING`, `lib/openal/BSD-3Clause`. Deliberately a separate DLL, not statically linked, so it can be replaced. |

## Not part of the core

The following are in the tree for other OpenJKDF2 targets and are not compiled
into the libretro build: GameNetworkingSockets, protobuf, PhysicsFS, curl,
mbedtls, drmingw.
