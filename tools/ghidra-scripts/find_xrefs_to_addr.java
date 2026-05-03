// Find all references to a specific address.
// @category MMgc
// @author claude-autonomous
//
// Usage: -postScript find_xrefs_to_addr.java 0xceb0bf

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class find_xrefs_to_addr extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long target = (args.length > 0) ? Long.decode(args[0]) : 0xceb0bfL;
        Address addr = currentProgram.getAddressFactory()
            .getDefaultAddressSpace().getAddress(target);
        println("=== xrefs to " + addr + " ===");
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceIterator refIter = currentProgram.getReferenceManager().getReferencesTo(addr);
        int n = 0;
        while (refIter.hasNext() && n < 50) {
            Reference ref = refIter.next();
            Address from = ref.getFromAddress();
            Function fn = fm.getFunctionContaining(from);
            String fnName = (fn != null) ? fn.getName() + "@" + fn.getEntryPoint() : "(no function)";
            println("  ref " + from + " in " + fnName + " (" + ref.getReferenceType() + ")");
            n++;
        }
        if (n == 0) println("  NO REFERENCES");
    }
}
