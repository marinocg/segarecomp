ARG BASE=segarecomp-xplat/tc-gnu:1
FROM ${BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends lhasa && rm -rf /var/lib/apt/lists/*
COPY vbcc0_9g.tar.gz vbcc_target_m68k-atari.lha vbcc_unix_config.tar /src/
RUN cd /src && echo "49848aec62b756d495a47010774710fb3e8b2d16e7272675386398bd4823f972  vbcc0_9g.tar.gz" | sha256sum -c && \
    echo "c626869c771227e0a27adfc809e3843c582c099831208d3d9f01e1aedbe690e9  vbcc_target_m68k-atari.lha" | sha256sum -c && \
    echo "d8563016adefd3ef84b7a1e33e6a552feb978c16b5f63ec917861d72a531574e  vbcc_unix_config.tar" | sha256sum -c && \
    tar xzf vbcc0_9g.tar.gz && cd vbcc && mkdir -p bin && (yes "" | make TARGET=m68k >/tmp/vbcc-make.log 2>&1) && (yes "" | make TARGET=m68ks >>/tmp/vbcc-make.log 2>&1) && \
    mkdir -p /opt/vbcc/bin && cp bin/vbccm68k bin/vbccm68ks bin/vc bin/vprof /opt/vbcc/bin/ && cp /usr/local/bin/vasmm68k_mot /usr/local/bin/vlink /opt/vbcc/bin/ && \
    cd /opt/vbcc && lha xq /src/vbcc_target_m68k-atari.lha && \
    mkdir -p targets && cp -r vbcc_target_m68k-atari/targets/* targets/ && mkdir -p config && cp -r vbcc_target_m68k-atari/config/* config/ && tar xf /src/vbcc_unix_config.tar && cat config/tos16
ENV VBCC=/opt/vbcc PATH=/opt/vbcc/bin:$PATH
