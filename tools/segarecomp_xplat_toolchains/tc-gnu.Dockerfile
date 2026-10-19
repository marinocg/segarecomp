FROM ubuntu@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-m68k-linux-gnu binutils-m68k-linux-gnu build-essential make python3 git ca-certificates xxd file \
 && rm -rf /var/lib/apt/lists/*
COPY vasm.tar.gz vlink.tar.gz /tmp/
RUN echo "c84b2de1cbb87831795fe64a85c5d9a7002a766e3a7c30b0a2d7d5e99d878f49  /tmp/vasm.tar.gz" | sha256sum -c && \
    echo "8d151cdd30a4feb575a364e68810c2bc300fe1a7c074dbbee6fd1175a6c5bfae  /tmp/vlink.tar.gz" | sha256sum -c && \
    cd /tmp && tar xzf vasm.tar.gz && cd vasm && make CPU=m68k SYNTAX=mot >/dev/null && cp vasmm68k_mot vobjdump /usr/local/bin/ && \
    make CPU=m68k SYNTAX=std >/dev/null && cp vasmm68k_std /usr/local/bin/ && \
    cd /tmp && tar xzf vlink.tar.gz && cd vlink && make >/dev/null && cp vlink /usr/local/bin/ && rm -rf /tmp/vasm* /tmp/vlink*
RUN git config --system --add safe.directory '*'
