# A68K 2.71 (Charlie Gibbs, public domain; Aminet dev/asm/A68kGibbs.lha) built from its 1991 C source.
ARG BASE=segarecomp-xplat/tc-vbcc:1
FROM ${BASE}
COPY A68kGibbs.lha /src/
RUN cd /src && echo "bc04d9b8c19a9d9500a3bc457bc5f596392326446c24312f5fc51e18791b5a32  A68kGibbs.lha" | sha256sum -c && \
    mkdir a68k && cd a68k && lha xq ../A68kGibbs.lha && \
    gcc -std=gnu89 -w -O1 -D__NOPROTO -o /usr/local/bin/a68k A68kmain.c A68kmisc.c Adirect.c Codegen.c Opcodes.c Operands.c Symtab.c && \
    cd / && rm -rf /src
