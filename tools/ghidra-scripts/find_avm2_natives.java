// Ghidra headless script — find AVM2 native method implementations in libCore.so
//
// Strategy: AVM2 native methods are registered via NativeInfo structs in
// .data.rel.ro that point to .text functions. Each class has a list of method
// names + function pointers. We look for the table containing pointers to
// our target strings (addChild, addEventListener, etc.) and report the
// adjacent function pointers.
//
// Usage:
//   analyzeHeadless <project_dir> <project_name> -postScript find_avm2_natives.java -noanalysis
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.data.PointerDataType;

import java.util.ArrayList;
import java.util.List;

public class find_avm2_natives extends GhidraScript {

    private static final String[] TARGET_STRINGS = {
        "addChild",
        "addChildAt",
        "removeChild",
        "removeChildAt",
        "removeChildren",
        "addEventListener",
        "removeEventListener",
    };

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("=== AVM2 Native Method Finder ===");

        for (String target : TARGET_STRINGS) {
            findString(target);
        }
    }

    private void findString(String target) throws Exception {
        Memory mem = currentProgram.getMemory();
        // Search for null-terminated string in all initialized memory.
        Address strAddr = mem.findBytes(
            mem.getMinAddress(),
            ("\0" + target + "\0").getBytes("ASCII"),
            null, true, monitor);
        if (strAddr == null) {
            println("  NOT FOUND: \"" + target + "\"");
            return;
        }
        // Skip the leading \0 we used as boundary.
        strAddr = strAddr.add(1);
        println("\n=== \"" + target + "\" at " + strAddr + " ===");

        // Find references to this string.
        ReferenceIterator refIter = currentProgram.getReferenceManager().getReferencesTo(strAddr);
        int refCount = 0;
        while (refIter.hasNext() && refCount < 20) {
            Reference ref = refIter.next();
            Address from = ref.getFromAddress();
            Function fn = currentProgram.getFunctionManager().getFunctionContaining(from);
            String fnName = (fn != null) ? fn.getName() + "@" + fn.getEntryPoint() : "(no function)";
            println("  ref from " + from + " in " + fnName);
            refCount++;
        }
        if (refCount == 0) {
            println("  NO XREFS — string not directly referenced (dispatch via index?)");
        }

        // Also scan .data.rel.ro for 8-byte pointers to this string (in case
        // Ghidra didn't auto-detect the references).
        Address found = null;
        Address scanStart = mem.getMinAddress();
        long target_le = strAddr.getOffset();
        byte[] needle = new byte[8];
        for (int i = 0; i < 8; i++) {
            needle[i] = (byte) (target_le >> (i * 8));
        }
        scanStart = mem.findBytes(scanStart, needle, null, true, monitor);
        int scanCount = 0;
        while (scanStart != null && scanCount < 5) {
            println("  raw 8-byte ptr at " + scanStart + " — adjacent qwords:");
            try {
                long prev = mem.getLong(scanStart.subtract(8));
                long next1 = mem.getLong(scanStart.add(8));
                long next2 = mem.getLong(scanStart.add(16));
                println(String.format("    [-8]=0x%016x  [+0]=0x%016x(str)  [+8]=0x%016x  [+16]=0x%016x",
                    prev, target_le, next1, next2));
                // Check if next1 looks like a function pointer
                Address candidate = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(next1);
                Function adjFn = currentProgram.getFunctionManager().getFunctionAt(candidate);
                if (adjFn != null) {
                    println("    --> POINTS TO FUNCTION: " + adjFn.getName());
                }
            } catch (Exception e) {
                println("    (read error: " + e.getMessage() + ")");
            }
            scanStart = mem.findBytes(scanStart.add(1), needle, null, true, monitor);
            scanCount++;
        }
    }
}
