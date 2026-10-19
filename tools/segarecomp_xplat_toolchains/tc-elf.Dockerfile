ARG BASE=segarecomp-xplat/tc-gnu:1
FROM ${BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libgmp-dev libmpfr-dev libmpc-dev texinfo xz-utils flex bison && rm -rf /var/lib/apt/lists/*
COPY binutils-2.42.tar.xz gcc-13.3.0.tar.xz /src/
RUN cd /src && echo "f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800  binutils-2.42.tar.xz" | sha256sum -c && \
    echo "0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083  gcc-13.3.0.tar.xz" | sha256sum -c && \
    tar xf binutils-2.42.tar.xz && tar xf gcc-13.3.0.tar.xz && \
    mkdir bb && cd bb && ../binutils-2.42/configure --target=m68k-elf --prefix=/opt/m68k-elf --disable-nls --disable-werror >/dev/null && make -j16 >/dev/null && make install >/dev/null && \
    cd /src && mkdir gb && cd gb && PATH=/opt/m68k-elf/bin:$PATH ../gcc-13.3.0/configure --target=m68k-elf --prefix=/opt/m68k-elf --enable-languages=c --without-headers --with-newlib --disable-nls --disable-libssp --disable-shared --disable-threads --with-cpu=68000 >/dev/null && \
    PATH=/opt/m68k-elf/bin:$PATH make -j16 all-gcc all-target-libgcc >/dev/null && PATH=/opt/m68k-elf/bin:$PATH make install-gcc install-target-libgcc >/dev/null && \
    rm -rf /src
ENV PATH=/opt/m68k-elf/bin:$PATH
