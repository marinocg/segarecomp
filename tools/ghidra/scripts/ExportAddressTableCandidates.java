// SEG-007-T204 / ADR-0033: deterministic offline whole-ROM Ghidra `Create
// Address Tables` analyzer export. Headless post-script, run as a SIBLING to
// (never instead of) ExportCodeEntryCandidates.java. Emits every table
// Ghidra's own address-table analyzer already materialised as
// `address_table_candidate` records -- explicitly NON-AUTHORITATIVE raw
// material. No new authority path, no ADR-0023 trust: a record here becomes
// a trusted `logical_table_descriptor` only via the C++
// `apply_genesis_address_table_corroboration` independent structural
// corroboration check (ADR-0033), and only when it independently agrees with
// a second, differently-sourced signal (the existing unweakened ADR-0025
// per-entry candidate validation walk applied as a self-terminating prefix).
//
// EMPIRICAL NOTE (SEG-007-T204 Phase 1 probe against the pinned recipe /
// Ghidra 12.1): the "Create Address Tables" analyzer does NOT materialise
// its result as a single `Array` `Data` item. It instead places one
// `Bookmark` (type "Analysis", category "Address Table") at each detected
// table's base address, and separately writes N consecutive individual
// pointer-sized `Data` items (one per entry) starting there -- confirmed
// empirically: the bookmark's own reported count and the length of the
// contiguous pointer-typed `Data` run starting at the bookmark address agree
// exactly in every sample inspected. This script therefore locates table
// bases via the bookmark (the analyzer's own genuine detection signal, not a
// guess) and independently measures the extent via the contiguous
// pointer-typed `Data` run -- itself already a small internal cross-check
// before this raw material is ever handed to the real (C++-side)
// corroboration mechanism.
//
// scriptArgs: <expected_rom_sha256> <output_path>
//
//  - Computes SHA-256 over the imported ROM bytes (same rule as
//    ExportCodeEntryCandidates.java).
//  - On hash mismatch: writes a valid artifact with an EMPTY candidate list.
//  - Otherwise scans every "Analysis"/"Address Table" bookmark in the
//    default address space, measures the contiguous run of 4-byte pointer-
//    typed `Data` items starting at that address, filters to a 1..256
//    entry-count bound (the existing project-wide code-pointer-table cap)
//    and an even base address, and emits one record per surviving table:
//    base address, entry width (4), stride (4), and entry count. Sorted by
//    base address; de-duplicated by base address.
//  - The `tools/ghidra.py analyze` wrapper canonically re-serialises this
//    file exactly like the code-entry-candidate export.
//
//@category segarecomp
import java.io.IOException;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.Iterator;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkManager;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;

public class ExportAddressTableCandidates extends GhidraScript {

    private static final int MAX_ENTRIES = 256;
    private static final int ENTRY_WIDTH_BYTES = 4;
    private static final String BOOKMARK_TYPE = "Analysis";
    private static final String BOOKMARK_CATEGORY = "Address Table";

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException(
                "usage: ExportAddressTableCandidates <expected_rom_sha256> <output_path>");
        }
        String expectedSha = args[0].trim().toLowerCase();
        String outputPath = args[1];

        String actualSha = romSha256();
        boolean hashMatch = expectedSha.equals(actualSha);

        TreeMap<Long, Integer> tables = new TreeMap<>();
        if (hashMatch) {
            collect(tables);
        }

        writeArtifact(outputPath, expectedSha, hashMatch, tables);
        println("ExportAddressTableCandidates: hash_match=" + hashMatch
            + " candidates=" + tables.size());
    }

    private String romSha256() throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isInitialized()) {
                continue;
            }
            byte[] buffer = new byte[65536];
            long remaining = block.getSize();
            Address cursor = block.getStart();
            while (remaining > 0) {
                int chunk = (int) Math.min(buffer.length, remaining);
                int read = block.getBytes(cursor, buffer, 0, chunk);
                if (read <= 0) {
                    break;
                }
                digest.update(buffer, 0, read);
                remaining -= read;
                cursor = cursor.add(read);
            }
        }
        byte[] out = digest.digest();
        StringBuilder hex = new StringBuilder(out.length * 2);
        for (byte b : out) {
            hex.append(Character.forDigit((b >> 4) & 0xF, 16));
            hex.append(Character.forDigit(b & 0xF, 16));
        }
        return hex.toString();
    }

    private boolean isPointerElement(DataType dataType) {
        if (dataType instanceof Pointer) {
            return ((Pointer) dataType).getLength() == ENTRY_WIDTH_BYTES;
        }
        return false;
    }

    // Measures the contiguous run of 4-byte pointer-typed `Data` items
    // starting exactly at `base` -- the same measurement Ghidra's own
    // analyzer already performed to produce its bookmark comment (see the
    // class-level note). Never trusts the comment text itself (a free-form,
    // potentially localized string); re-derives the count structurally.
    private int contiguousPointerRun(Listing listing, Address base) {
        int count = 0;
        Address cursor = base;
        while (count < MAX_ENTRIES + 1) {
            Data data = listing.getDataAt(cursor);
            if (data == null || !isPointerElement(data.getDataType())) {
                break;
            }
            count++;
            cursor = cursor.add(ENTRY_WIDTH_BYTES);
        }
        return count;
    }

    private void collect(TreeMap<Long, Integer> out) {
        Listing listing = currentProgram.getListing();
        BookmarkManager bookmarks = currentProgram.getBookmarkManager();
        Iterator<Bookmark> iterator = bookmarks.getBookmarksIterator();
        while (iterator.hasNext() && !monitor.isCancelled()) {
            Bookmark bookmark = iterator.next();
            if (!BOOKMARK_TYPE.equals(bookmark.getTypeString()) ||
                    !BOOKMARK_CATEGORY.equals(bookmark.getCategory())) {
                continue;
            }
            Address base = bookmark.getAddress();
            if (base == null || !base.getAddressSpace()
                    .equals(currentProgram.getAddressFactory().getDefaultAddressSpace())) {
                continue;
            }
            long offset = base.getOffset();
            if (offset < 0 || (offset & 1L) != 0L) {
                continue;
            }
            int count = contiguousPointerRun(listing, base);
            if (count < 1 || count > MAX_ENTRIES) {
                continue;
            }
            out.putIfAbsent(offset, count);
        }
    }

    private void writeArtifact(String path, String romSha, boolean hashMatch, TreeMap<Long, Integer> tables)
            throws IOException {
        StringBuilder json = new StringBuilder();
        json.append("[");
        boolean first = true;
        if (hashMatch) {
            for (java.util.Map.Entry<Long, Integer> entry : tables.entrySet()) {
                if (!first) {
                    json.append(",");
                }
                first = false;
                json.append("{\"kind\":\"address_table_candidate\",\"rom_sha256\":\"")
                    .append(romSha)
                    .append("\",\"base_address\":")
                    .append(entry.getKey().longValue())
                    .append(",\"entry_width_bytes\":")
                    .append(ENTRY_WIDTH_BYTES)
                    .append(",\"stride_bytes\":")
                    .append(ENTRY_WIDTH_BYTES)
                    .append(",\"entry_count\":")
                    .append(entry.getValue().intValue())
                    .append(",\"provenance\":{\"tool\":\"ghidra\",\"tool_version\":")
                    .append("\"SEG-007-T204-ADR-0033\",\"timestamp\":\"1970-01-01T00:00:00Z\",")
                    .append("\"human_reviewed\":false}}");
            }
        }
        json.append("]\n");
        try (Writer writer = Files.newBufferedWriter(Paths.get(path), StandardCharsets.UTF_8)) {
            writer.write(json.toString());
        }
    }
}
