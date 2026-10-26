// List every instruction that references the given addresses (reads, writes, calls), with its function.
// Headless usage: -postScript XrefsTo.java <output.txt> <addr> [<addr> ...]
// @category MK3D
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0], true));
        for (int i = 1; i < args.length; i++) {
            String text = args[i].startsWith("0x") ? args[i].substring(2) : args[i];
            Address target = toAddr(Long.parseLong(text, 16));
            out.println("// ===== references to " + target);
            int n = 0;
            for (Reference r : getReferencesTo(target)) {
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction insn = getInstructionAt(from);
                out.println(String.format("%s  %-10s %-28s %s", from, r.getReferenceType(),
                        f == null ? "?" : f.getName() + "@" + f.getEntryPoint(), insn == null ? "" : insn.toString()));
                n++;
            }
            out.println("// " + n + " references");
        }
        out.close();
    }
}
