// Ghidra headless — AVM2 native method finder v3.
// Pure raw byte-substring search; reports ALL occurrences with xref enumeration.
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.HashSet;
import java.util.Set;

public class find_avm2_natives_v3 extends GhidraScript {

    private static final String[] TARGET_STRINGS = {
        "addChild",
        "addChildAt",
        "removeChild",
        "removeChildAt",
        "removeChildren",
        "addEventListener",
        "removeEventListener",
        "dispatchEvent",
        "hasEventListener",
        "willTrigger",
    };

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("=== AVM2 Native Method Finder v3 ===");

        for (String target : TARGET_STRINGS) {
            println("\n=== \"" + target + "\" ===");
            byte[] needle = target.getBytes("ASCII");
            Set<Long> hits = new HashSet<>();
            Address start = mem.getMinAddress();
            int safetyCount = 0;
            while (safetyCount < 100) {
                Address found = mem.findBytes(start, needle, null, true, monitor);
                if (found == null) break;
                hits.add(found.getOffset());
                start = found.add(1);
                safetyCount++;
            }
            println("  total raw matches: " + hits.size());
            for (Long off : hits) {
                Address addr = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(off);
                MemoryBlock blk = mem.getBlock(addr);
                String blkName = (blk != null) ? blk.getName() : "?";
                // Char before
                String prev = "?";
                try {
                    byte b = mem.getByte(addr.subtract(1));
                    prev = (b == 0) ? "\\0" : (b >= 0x20 && b < 0x7f) ? String.valueOf((char) b) : String.format("\\x%02x", b);
                } catch (Exception e) { /* ignore */ }
                String suff = "?";
                try {
                    byte b = mem.getByte(addr.add(target.length()));
                    suff = (b == 0) ? "\\0" : (b >= 0x20 && b < 0x7f) ? String.valueOf((char) b) : String.format("\\x%02x", b);
                } catch (Exception e) { /* ignore */ }
                println(String.format("  %s [%s] prev=%s suff=%s", addr, blkName, prev, suff));

                // Enumerate xrefs
                ReferenceIterator refIter = currentProgram.getReferenceManager().getReferencesTo(addr);
                int refN = 0;
                while (refIter.hasNext() && refN < 10) {
                    Reference ref = refIter.next();
                    Address from = ref.getFromAddress();
                    Function fn = fm.getFunctionContaining(from);
                    String fnName = (fn != null) ? fn.getName() + "@" + fn.getEntryPoint() : "(none)";
                    println("    ref " + from + " in " + fnName);
                    refN++;
                }
            }
        }
    }
}
