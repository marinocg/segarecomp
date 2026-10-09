# Vendored zlib 1.3.1 subset (SEG-047, ADR 0096)

Purpose: reproduce `zlib.compress(window, 6)` lengths for the frozen SEG-046 `b_zlib` feature identically on every supported platform.
Source: the zlib 1.3.1 C sources (https://www.zlib.net/, zlib license, see `zlib.h`), the subset needed for single-shot level-6
`deflate` in the zlib wrapper: `adler32.c deflate.c trees.c zutil.c` and their headers. The files are verbatim from the zlib 1.3.1
release except for a provenance comment added by the redistributor at the top of each file. `segarecomp_crc32.c` is the only file
written for this project (see the comment in it). Not built into generated programs or the runtime; only the native region producer links it.

License: `LICENSE` in this directory is the zlib license text copied from `zlib.h`; releases ship it as `licenses/zlib.txt` and list it in `packaging/THIRD-PARTY-NOTICES.txt`.

SHA-256 of the vendored files (drift-checked by `tests/segarecomp_ml_region_test.py`):

```
d2af81a407f981ca99f88ce96c96920f0fa4687450b9f19b094411a9b90b15bc  adler32.c
86c2bb5370b892e4d995fc14661af48353dcd53994693534364dba059a7a428a  deflate.c
6294259c16d84f9125ecdcc10a37a09e0bd5fc8b4ebaeeb6c8415ecda3525c70  deflate.h
d772c783884af9c42a429ce7912b53f1b56281df7517fad2e9e9887bde89860d  gzguts.h
24dee2a43a0511521962dec1a9b5a8eacf00e5cab50e6d90802d775e639bc417  trees.c
44268612648ff3a69841d8a36cb78f793b751c5c47d2ef3f2f7a69ad11948e2d  trees.h
4f73e2d80113813452f7e3c0117e0847db323902a795902dca0466d3488e53a7  zconf.h
5b3dc1c20e9b852ec1ef77f598c89129bae58de98d3773f91a4fc3071dd8cabf  zlib.h
c368365bfb0325ac2c8dae3bb9b503daf371515707f020c54060e38ceb4d1206  zutil.c
d757abb6e1bfe3fbc97f8179a8ca9865988a5eafacfe6ee48b5c6de6f18be8de  zutil.h
```
