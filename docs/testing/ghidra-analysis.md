# Optional Ghidra analysis

`tools/ghidra.py` manages a pinned, containerized, headless Ghidra plus a Ghidra MCP bridge. It is a
developer aid for research only: Ghidra output is **not** an instruction-semantics correctness oracle.

## Stack

- `ghidra`: Ghidra 12.1.2 plus the GhidraMCP v6.0.0 headless REST server;
- `bridge`: the checksum-verified v6.0.0 Python MCP bridge, used over stdio.

The upstream source is pinned to commit `8cd2078e10b9ba28b188cb84ce5b9051a904b995`. The bridge wheel is
SHA-256 verified and its Python dependency versions are constrained in `tools/ghidra/bridge-constraints.txt`.
Both services run as unprivileged users, no port is published to the host, a generated 256-bit token
authenticates bridge-to-Ghidra requests, arbitrary Ghidra scripts are disabled, and only
`.tools/ghidra/input` is mounted read-only for imports. The runtime/input root (`.tools/ghidra`) is
ignored by Git and is derived from the repository's Git common directory, so every linked worktree shares
one stack.

The pinned bridge only connects to a loopback URL, so the `bridge` service joins the `ghidra` service
network namespace (`network_mode: service:ghidra`) and uses `http://127.0.0.1:8089`; the loopback
validation is not weakened. No Docker socket, repository, home directory or credential path is mounted;
`cap_drop: [ALL]`, `no-new-privileges` and a read-only bridge filesystem stay in force.

## Start

```sh
python3 tools/ghidra.py init
# Place only legally usable binaries in .tools/ghidra/input/
python3 tools/ghidra.py config
python3 tools/ghidra.py up
python3 tools/ghidra.py health
```

The first build downloads Ghidra and compiles the pinned upstream headless server. On Apple Silicon the
stack intentionally runs as `linux/amd64`, so startup and analysis are slower under emulation. An MCP
client starts the bridge with:

```sh
python3 tools/ghidra.py stdio
```

## Operate

```sh
python3 tools/ghidra.py status
python3 tools/ghidra.py logs ghidra
python3 tools/ghidra.py logs bridge
python3 tools/ghidra.py down
```

`down` preserves the named Ghidra data/project volumes; the tool has no automatic volume-deletion command.

## Security and evidence

- Treat every analyzed binary as untrusted and keep Docker Desktop/Engine updated.
- Do not mount the repository, home directory, Docker socket, ROM collection, or credential paths.
- Do not enable `GHIDRA_MCP_ALLOW_SCRIPTS` without a reviewed requirement.
- Record the GhidraMCP version, upstream commit, image IDs, loader/language choice, and relevant analysis
  options when using output as evidence.
- Review upstream changes and update source, wheel URL, SHA-256, tests, and this document together.
