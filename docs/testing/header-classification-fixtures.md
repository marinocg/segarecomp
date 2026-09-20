# Header-classification fixtures

`tests/fixtures/header-classification-fixtures.json` is the authoritative manifest for the
bounded header-classification test corpus.  Every entry records its nonempty `purpose`, declarative
construction and `size`, and exact SHA-256. Each byte array starts as zeroes, except entries with a
recorded deterministic Python `random.Random` seed. Its construction overwrites only the listed
header bytes; it contains no ROM, firmware, key, or derived material.

`tests/ingestion_cli_test.py` materializes every manifest entry in a temporary directory, verifies
its nonempty purpose and recorded SHA-256, and runs both public command paths.  The matrix covers all rows in
`header-classification-contract.md`, including all unsupported region nibbles and every required
field truncation boundary.  The eight `random-*` fixtures are seeded signature-background cases.

Run the focused fixture oracle after building:

```sh
python3 tests/ingestion_cli_test.py build/dev/segarecomp
```

The corpus classifies header bytes only.  It makes no mapper, checksum, executable-map, reset,
bootability, decode, or runtime claim.
