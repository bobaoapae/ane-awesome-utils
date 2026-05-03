// Scan .text for AArch64 BL instructions targeting a specific address.
// Reports caller PC + the function it lives in.
//
// AArch64 BL encoding: 0b100101_imm26  (top 6 bits = 100101)
//   target = pc + sign_extend(imm26 << 2)
//
// Usage: -postScript find_bl_calls_to.java 0x896824
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class find_bl_calls_to extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long target = (args.length > 0) ? Long.decode(args[0]) : 0x896824L;
        println("=== AArch64 BL scan targeting 0x" + Long.toHexString(target) + " ===");
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        int hits = 0;
        for (MemoryBlock blk : mem.getBlocks()) {
            if (!blk.isExecute() || !blk.isInitialized()) continue;
            println("scanning block " + blk.getName() + " " + blk.getStart() + ".." + blk.getEnd());
            long start = blk.getStart().getOffset();
            long end = blk.getEnd().getOffset();
            // 4-byte aligned
            for (long pc = start; pc + 4 <= end + 1; pc += 4) {
                int insn;
                try {
                    Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(pc);
                    insn = mem.getInt(a);
                } catch (Exception e) {
                    continue;
                }
                // BL: top 6 bits = 0b100101 => 0x94 << 24 to 0x97 << 24 (top byte 0x94..0x97)
                int top6 = (insn >>> 26) & 0x3f;
                if (top6 != 0x25) continue;
                // imm26 sign-extended
                long imm26 = insn & 0x3ffffff;
                if ((imm26 & 0x2000000) != 0) imm26 |= 0xfffffffffc000000L;  // sign extend
                long branchTarget = pc + (imm26 << 2);
                if (branchTarget == target) {
                    Address from = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(pc);
                    Function fn = fm.getFunctionContaining(from);
                    String fnName = (fn != null) ? fn.getName() + "@0x" + Long.toHexString(fn.getEntryPoint().getOffset()) : "(no function)";
                    println(String.format("  BL at 0x%08x  in %s", pc, fnName));
                    hits++;
                }
            }
        }
        println("=== total BL callers: " + hits + " ===");
    }
}
