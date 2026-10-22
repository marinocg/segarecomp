// SEG-077-T009: bounded export of every instruction Ghidra disassembled (address and length), for measurement against source truth.
// Headless post-script. scriptArgs: <output_path>. Output: JSON [[address, length], ...] sorted, default space only.
//@category segarecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.FileWriter;

public class ExportInstructionStarts extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String out = getScriptArgs()[0];
        StringBuilder sb = new StringBuilder("[");
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        boolean first = true;
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (!ins.getAddress().getAddressSpace().isMemorySpace()) continue;
            if (!first) sb.append(",");
            sb.append("[").append(ins.getAddress().getOffset()).append(",").append(ins.getLength()).append("]");
            first = false;
        }
        sb.append("]");
        try (FileWriter w = new FileWriter(out)) { w.write(sb.toString()); }
    }
}
