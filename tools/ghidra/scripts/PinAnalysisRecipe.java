// SEG-007-T179 / ADR-0025: deterministic analyzer recipe. Headless pre-script,
// run before auto-analysis. Enables the recall-relevant analyzers and disables
// the ones whose ordering/heuristics threaten byte-identical determinism across
// clean runs. Missing option names are ignored (Ghidra version tolerance).
//
//@category segarecomp
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.listing.Program;

public class PinAnalysisRecipe extends GhidraScript {

    private static final String[] ENABLE = {
        "Disassemble Entry Points",
        "Aggressive Instruction Finder",
        "Function Start Search",
        "Basic Constant Reference Analyzer",
        "Reference",
        "Create Address Tables",
    };

    private static final String[] DISABLE = {
        "Decompiler Parameter ID",
        "Decompiler Switch Analysis",
        "Shared Return Calls",
        "Non-Returning Functions - Discovered",
    };

    @Override
    protected void run() throws Exception {
        Options options = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
        for (String name : options.getOptionNames()) {
            for (String want : ENABLE) {
                if (name.equals(want) || name.endsWith("." + want)) {
                    trySetBoolean(options, name, true);
                }
            }
            for (String want : DISABLE) {
                if (name.equals(want) || name.endsWith("." + want)) {
                    trySetBoolean(options, name, false);
                }
            }
        }
        println("PinAnalysisRecipe: applied deterministic analyzer recipe");
    }

    private void trySetBoolean(Options options, String name, boolean value) {
        try {
            options.setBoolean(name, value);
        } catch (RuntimeException ignored) {
            // Option is not a boolean / not settable on this version -- skip.
        }
    }
}
