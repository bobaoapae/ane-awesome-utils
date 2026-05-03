// Ghidra headless — find AVM2 native method implementations in libCore.so v2.
//
// Improvements over v1:
//   1. Relaxed string search: find target as substring anywhere in .rodata
//      (Adobe concatenates AS3 method names; \\0XXX\\0 boundary fails on those)
//   2. ALL xrefs reported, not capped at 20
//   3. Byte-pattern scan in .data.rel.ro for AVM2 native method tables:
//      Adobe's NativeInfo struct typically has {abc_method_index, fn_ptr}
//      sequence. We scan for any 8-byte qword in .data.rel.ro that points
//      into .text (= function pointer) AND whose preceding/following qword
//      points to a string in .rodata containing our target.
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.RefType;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class find_avm2_natives_v2 extends GhidraScript {

    private static final String[] TARGET_STRINGS = {
        "addChild",
        "addChildAt",
        "removeChild",
        "removeChildAt",
        "removeChildren",
        "addEventListener",
        "removeEventListener",
        "dispatchEvent",
        "willTrigger",
        "hasEventListener",
    };

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("=== AVM2 Native Method Finder v2 ===");

        // Scope text section for function-pointer validation
        AddressSetView textRange = getTextRange();
        println("text range: " + textRange);

        // Scope rodata for string-pointer validation (only initialized read-only)
        AddressSetView roRange = getReadOnlyRange();
        println("ro range: " + roRange);

        for (String target : TARGET_STRINGS) {
            findStringMatches(target, textRange, roRange);
        }
    }

    private AddressSetView getTextRange() {
        AddressSet result = new AddressSet();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isExecute() && b.isInitialized()) {
                result.addRange(b.getStart(), b.getEnd());
            }
        }
        return result;
    }

    private AddressSetView getReadOnlyRange() {
        AddressSet result = new AddressSet();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isInitialized() && b.isRead() && !b.isWrite() && !b.isExecute()) {
                result.addRange(b.getStart(), b.getEnd());
            }
        }
        return result;
    }

    private void findStringMatches(String target, AddressSetView textRange, AddressSetView roRange) throws Exception {
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();

        println("\n=== \"" + target + "\" ===");

        // Search ALL initialized memory for the substring (no boundary requirement).
        Set<Long> foundOffsets = new HashSet<>();
        Address start = mem.getMinAddress();
        byte[] needle = target.getBytes("ASCII");
        int matchCount = 0;
        while (matchCount < 30) {
            Address found = mem.findBytes(start, needle, null, true, monitor);
            if (found == null) break;
            // Only report if at start of a word OR preceded by null byte (likely real string)
            boolean isStringStart = false;
            try {
                if (found.getOffset() == 0) {
                    isStringStart = true;
                } else {
                    byte prev = mem.getByte(found.subtract(1));
                    isStringStart = (prev == 0);
                }
            } catch (Exception e) { /* ignore */ }

            if (isStringStart) {
                foundOffsets.add(found.getOffset());
                if (foundOffsets.size() <= 6) {
                    println("  string at " + found + (roRange.contains(found) ? " (.rodata)" : " (.text!)"));
                }
            }
            start = found.add(1);
            matchCount++;
        }
        if (foundOffsets.isEmpty()) {
            println("  NOT FOUND");
            return;
        }

        // For each string occurrence, find xrefs.
        Set<Long> functionsHit = new HashSet<>();
        for (Long off : foundOffsets) {
            Address strAddr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(off);
            ReferenceIterator refIter = currentProgram.getReferenceManager().getReferencesTo(strAddr);
            while (refIter.hasNext()) {
                Reference ref = refIter.next();
                Address from = ref.getFromAddress();
                Function fn = fm.getFunctionContaining(from);
                if (fn != null) {
                    functionsHit.add(fn.getEntryPoint().getOffset());
                }
            }
        }
        println("  total xref-containing functions: " + functionsHit.size());
        for (Long fnOff : functionsHit) {
            Address fnAddr = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(fnOff);
            Function fn = fm.getFunctionAt(fnAddr);
            String name = (fn != null) ? fn.getName() : "?";
            println("    " + fnAddr + "  " + name);
        }

        // Also: scan AVM2 native method tables. Look for 8-byte qwords in
        // .data sections that point to a string containing our target,
        // adjacent to a qword that points into .text (function pointer).
        scanAvm2NativeTables(target, foundOffsets, textRange);
    }

    private void scanAvm2NativeTables(String target, Set<Long> stringOffsets, AddressSetView textRange) throws Exception {
        Memory mem = currentProgram.getMemory();
        // Look at all writable initialized blocks (.data, .data.rel.ro, .got)
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized()) continue;
            if (b.isExecute()) continue;
            String name = b.getName();
            if (!name.startsWith(".data") && !name.equals(".got")) continue;

            Address cur = b.getStart();
            Address end = b.getEnd();
            int hits = 0;
            while (cur.getOffset() + 8 <= end.getOffset() && hits < 5) {
                long ptr;
                try {
                    ptr = mem.getLong(cur);
                } catch (Exception e) {
                    cur = cur.add(8);
                    continue;
                }
                // Is this a pointer to one of our string occurrences?
                // Or pointer that, when read as cstring, contains the target?
                boolean isStrPtr = false;
                if (stringOffsets.contains(ptr)) {
                    isStrPtr = true;
                } else if (ptr > 0 && ptr < 0x10000000L) {
                    // Try reading 32 bytes as ASCII to see if it contains target
                    try {
                        Address strAddr = currentProgram.getAddressFactory()
                            .getDefaultAddressSpace().getAddress(ptr);
                        byte[] sample = new byte[40];
                        mem.getBytes(strAddr, sample);
                        String s = new String(sample, "ASCII");
                        if (s.startsWith(target)) {
                            isStrPtr = true;
                        }
                    } catch (Exception e) { /* ignore */ }
                }
                if (isStrPtr) {
                    // Check adjacent qwords for function pointer.
                    long[] adj = new long[6];  // -3..+3
                    for (int i = 0; i < 6; i++) {
                        try {
                            adj[i] = mem.getLong(cur.add((i - 3) * 8));
                        } catch (Exception e) {
                            adj[i] = 0;
                        }
                    }
                    println("  TABLE @ " + cur + " (block " + name + "):");
                    for (int i = 0; i < 6; i++) {
                        Address a = currentProgram.getAddressFactory()
                            .getDefaultAddressSpace().getAddress(adj[i]);
                        boolean inText = textRange.contains(a);
                        String tag = (i == 3) ? "(str)" : (inText ? "(*** TEXT ***)" : "");
                        Function adjFn = inText ? currentProgram.getFunctionManager().getFunctionContaining(a) : null;
                        String fnName = (adjFn != null) ? "  → " + adjFn.getName() + "@" + adjFn.getEntryPoint() : "";
                        println(String.format("    [%+d]=0x%016x %s%s", i - 3, adj[i], tag, fnName));
                    }
                    hits++;
                }
                cur = cur.add(8);
            }
        }
    }
}
