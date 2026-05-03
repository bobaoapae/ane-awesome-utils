// Dump N bytes at given address as hex.
// Usage: -postScript dump_bytes_at.java 0x5541cc 256
//
// @category MMgc
// @author claude-autonomous

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class dump_bytes_at extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long off = (args.length > 0) ? Long.decode(args[0]) : 0x5541ccL;
        int n   = (args.length > 1) ? Integer.decode(args[1]) : 128;
        Memory mem = currentProgram.getMemory();
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(off);
        byte[] buf = new byte[n];
        try {
            mem.getBytes(a, buf);
        } catch (Exception e) {
            println("read failed: " + e.getMessage());
            return;
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            if (i % 16 == 0) {
                if (i > 0) sb.append('\n');
                sb.append(String.format("0x%08x:", off + i));
            }
            sb.append(String.format(" %02x", buf[i] & 0xff));
        }
        println(sb.toString());
    }
}
