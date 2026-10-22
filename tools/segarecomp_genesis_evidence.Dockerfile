# SEG-077-T001: isolated, offline-at-runtime evidence toolchain for segarecomp_genesis_evidence.py (local research image; never published).
#   MAME 0.276 (Debian trixie package 0.276+dfsg.1-1+deb13u1; GPL-2.0+, run as an external tool only) - independent execution tracer
#   Oxore/m68k-disasm @ bb6d83981bbb53de352061b793c8215e45af895a (Unlicense) - independent MC68000 decoder (traced + walk modes)
# Build: docker build -t segarecomp-seg077/evidence:1 -f tools/segarecomp_genesis_evidence.Dockerfile tools
# Containers run with --network none, read-only inputs and an unprivileged user.
FROM debian@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
RUN apt-get update && apt-get install -y --no-install-recommends mame=0.276+dfsg.1-1+deb13u1 ca-certificates git cmake make g++ python3 \
 && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/Oxore/m68k-disasm /opt/m68k-disasm && cd /opt/m68k-disasm \
 && git checkout bb6d83981bbb53de352061b793c8215e45af895a \
 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
RUN useradd -m -u 1000 tracer
USER tracer
WORKDIR /work
