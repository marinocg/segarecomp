// SEG-077-T009: Genesis-aware seeding for the raw BinaryLoader: mark the reset / exception / interrupt vector targets (longwords 1-63
// of the cartridge vector table) that lie inside the image at even addresses >= 0x200 as entry points and disassemble from them.
// Headless pre-script (no arguments). Generic hardware knowledge only; no title-specific addresses.
//@category segarecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class SeedGenesisVectors extends GhidraScript {
    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        long size = mem.getMaxAddress().getOffset() + 1;
        int seeded = 0;
        for (int i = 1; i < 64; i++) {
            long t = mem.getInt(toAddr(4L * i)) & 0xFFFFFFFFL;
            if (t >= 0x200 && t < size && (t & 1) == 0) {
                Address a = toAddr(t);
                currentProgram.getSymbolTable().addExternalEntryPoint(a);
                disassemble(a);
                seeded++;
            }
        }
        println("SeedGenesisVectors: seeded " + seeded);
    }
}
