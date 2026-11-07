Segarecomp native patch SDK (Genesis, ABI 1)
============================================

This folder ships with the release so a patch author can work from an installed copy.

  runtime/platforms/genesis/sdk/segarecomp_genesis_patch.h   the public provider header (the only header a provider is compiled against)
  sdk/native-patch-sdk.md                                    the provider contract, packaging and build/identity rules
  sdk/patch-packages.md                                      the .segapatch format (schema 1 data-only BPS, schema 2 native source), verification and trust
  sdk/sample/provider.c                                      a complete SYNTHETIC example provider (no commercial content)

Compile-check a provider exactly as the build does (strict C11, this header only), with the bundled compiler:

  toolchain/zig cc -std=c11 -Wall -Wextra -pedantic -I runtime/platforms/genesis/sdk -c provider.c -o provider.o

Packaging a .segapatch needs tools/segapatch.py from the source repository; it is an author tool and is not part of this package.

SECURITY: a patch with a native provider is executable code. It is NOT sandboxed and runs with the permissions of the game you start.
A hash proves the file is unchanged, not who wrote it or that it is safe. Install such a patch only from a source you trust.
No game-specific patch is included in this package.
