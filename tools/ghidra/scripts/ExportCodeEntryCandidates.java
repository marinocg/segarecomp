// SEG-007-T179 / ADR-0025: deterministic offline whole-ROM code-entry candidate
// exporter. Headless post-script. Emits the high-recall union of recognized
// code-entry addresses as ADR-0024 `code_entry_candidate` records in the
// heterogeneous `--external-hints` JSON format: addresses only, sorted and
// de-duplicated. No new record kind / parser / authority path.
//
// scriptArgs: <expected_rom_sha256> <output_path>
//
//  - Computes SHA-256 over the imported ROM bytes.
//  - On hash mismatch: writes a valid artifact with an EMPTY candidate list.
//  - Otherwise unions FunctionManager entries, code-block starts, instruction
//    run starts, non-computed flow-reference destinations, and code-island run
//    starts; filters to executable memory; keeps the default (program) space;
//    drops odd addresses; sorts ascending; de-dups.
//  - The `tools/ghidra.py analyze` wrapper canonically re-serialises this file.
//
//@category segarecomp
import java.io.IOException;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.block.CodeBlock;
import ghidra.program.model.block.CodeBlockIterator;
import ghidra.program.model.block.SimpleBlockModel;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportCodeEntryCandidates extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException(
                "usage: ExportCodeEntryCandidates <expected_rom_sha256> <output_path>");
        }
        String expectedSha = args[0].trim().toLowerCase();
        String outputPath = args[1];

        String actualSha = romSha256();
        boolean hashMatch = expectedSha.equals(actualSha);

        TreeSet<Long> candidates = new TreeSet<>();
        if (hashMatch) {
            collect(candidates);
        }

        writeArtifact(outputPath, expectedSha, hashMatch, candidates);
        println("ExportCodeEntryCandidates: hash_match=" + hashMatch
            + " candidates=" + candidates.size());
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

    private boolean executable(Address address) {
        MemoryBlock block = currentProgram.getMemory().getBlock(address);
        return block != null && block.isExecute() && block.isInitialized();
    }

    private void consider(TreeSet<Long> out, Address address) {
        if (address == null) {
            return;
        }
        if (!address.getAddressSpace()
                .equals(currentProgram.getAddressFactory().getDefaultAddressSpace())) {
            return;
        }
        if (!executable(address)) {
            return;
        }
        long offset = address.getOffset();
        if (offset < 0 || (offset & 1L) != 0L) {
            return;
        }
        out.add(offset);
    }

    private void collect(TreeSet<Long> out) throws Exception {
        Listing listing = currentProgram.getListing();

        FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
        while (functions.hasNext() && !monitor.isCancelled()) {
            Function fn = functions.next();
            consider(out, fn.getEntryPoint());
        }

        SimpleBlockModel blocks = new SimpleBlockModel(currentProgram);
        CodeBlockIterator blockIter = blocks.getCodeBlocks(monitor);
        while (blockIter.hasNext() && !monitor.isCancelled()) {
            CodeBlock block = blockIter.next();
            consider(out, block.getFirstStartAddress());
        }

        InstructionIterator instructions = listing.getInstructions(true);
        Instruction previous = null;
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction current = instructions.next();
            boolean contiguous = previous != null
                && previous.getMaxAddress().add(1).equals(current.getMinAddress());
            if (!contiguous) {
                consider(out, current.getMinAddress());
            }
            boolean currentInFunction = currentProgram.getFunctionManager()
                .getFunctionContaining(current.getAddress()) != null;
            boolean previousInFunction = previous != null && currentProgram.getFunctionManager()
                .getFunctionContaining(previous.getAddress()) != null;
            if (!currentInFunction && (!contiguous || previousInFunction)) {
                consider(out, current.getMinAddress());
            }
            previous = current;
        }

        ReferenceManager refs = currentProgram.getReferenceManager();
        AddressSetView executableSet = currentProgram.getMemory().getExecuteSet();
        AddressIterator sources = refs.getReferenceSourceIterator(executableSet, true);
        while (sources.hasNext() && !monitor.isCancelled()) {
            Address from = sources.next();
            for (Reference ref : refs.getReferencesFrom(from)) {
                if (!(ref.getReferenceType() instanceof FlowType)) {
                    continue;
                }
                FlowType flow = (FlowType) ref.getReferenceType();
                if (!flow.isFlow() || flow.isComputed()) {
                    continue;
                }
                consider(out, ref.getToAddress());
            }
        }
    }

    private void writeArtifact(String path, String romSha, boolean hashMatch, TreeSet<Long> candidates)
            throws IOException {
        StringBuilder json = new StringBuilder();
        json.append("[");
        boolean first = true;
        if (hashMatch) {
            for (Long offset : candidates) {
                if (!first) {
                    json.append(",");
                }
                first = false;
                json.append("{\"kind\":\"code_entry_candidate\",\"rom_sha256\":\"")
                    .append(romSha)
                    .append("\",\"address\":")
                    .append(offset.longValue())
                    .append(",\"provenance\":{\"tool\":\"ghidra\",\"tool_version\":")
                    .append("\"SEG-007-T179-ADR-0025\",\"timestamp\":\"1970-01-01T00:00:00Z\",")
                    .append("\"human_reviewed\":false}}");
            }
        }
        json.append("]\n");
        try (Writer writer = Files.newBufferedWriter(Paths.get(path), StandardCharsets.UTF_8)) {
            writer.write(json.toString());
        }
    }
}
