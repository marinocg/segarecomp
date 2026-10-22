# SEG-077-T009: offline decompression-evidence helper (sega2asm decompressors, MIT) - local research image, never published.
# Build: docker build -t segarecomp-seg077/decomp:1 -f tools/segarecomp_genesis_decomp.Dockerfile tools
FROM golang:1.22-bookworm AS build
RUN git clone https://github.com/hansbonini/sega2asm /src && cd /src && git checkout 78f37b0d71d27b43b960e24c01c5437bbef55404
COPY segarecomp_genesis_decomp/main.go /src/cmd/seg077decomp/main.go
RUN cd /src && CGO_ENABLED=0 go build -trimpath -o /seg077decomp ./cmd/seg077decomp
FROM debian:bookworm-slim
COPY --from=build /seg077decomp /usr/local/bin/seg077decomp
RUN useradd -m -u 1000 tracer
USER tracer
