// List functions defined near a given address. Quick way to verify Ghidra
// analysis ran and identified a function entry there.
//
// Usage: -postScript list_funcs_near.java 0x5541cc 0x10000
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class list_funcs_near extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long center = (args.length > 0) ? Long.decode(args[0]) : 0x5541cdL;
        long span   = (args.length > 1) ? Long.decode(args[1]) : 0x10000L;
        long lo = center - span/2;
        long hi = center + span/2;
        println("=== functions in [0x" + Long.toHexString(lo) + ", 0x" + Long.toHexString(hi) + "] ===");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function fn = it.next();
            long off = fn.getEntryPoint().getOffset();
            if (off < lo || off > hi) continue;
            println(String.format("  0x%08x  size=%d  %s",
                off, fn.getBody().getNumAddresses(), fn.getName()));
            n++;
            if (n >= 80) { println("  ... (capped)"); break; }
        }
        println("=== total: " + n + " ===");
    }
}
