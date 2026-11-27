Segarecomp native patch SDK (Genesis)
=====================================

ABI 1 is DEPRECATED, not removed: write new providers against ABI 2 (sdk/native-abi-migration.md, sdk/sample/abi2/).
Existing ABI 1 packages keep working. Links inside the shipped documents that point elsewhere in the source repository
resolve only there.

  runtime/platforms/genesis/sdk/segarecomp_mod_genesis.h     the ABI 2 Genesis provider header (with runtime/platforms/sdk/segarecomp_mod.h)
  sdk/native-abi-migration.md                                ABI 1 to ABI 2, one example per facility; compatibility policy
  sdk/sample/abi2/                                           a complete SYNTHETIC ABI 2 example project (schema 3)

The rest of this page describes the deprecated ABI 1 material.

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
