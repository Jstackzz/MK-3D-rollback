// Decompile the functions containing the given addresses to one text file, with their callers.
// Headless usage: -postScript DecompileFuncs.java <output.txt> <addr> [<addr> ...]
// @category MK3D
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        for (int i = 1; i < args.length; i++) {
            String text = args[i].startsWith("0x") ? args[i].substring(2) : args[i];
            Address addr = toAddr(Long.parseLong(text, 16));
            Function f = getFunctionContaining(addr);
            if (f == null) {  // the analysis never reached it (code only called through pointers): make one there
                disassemble(addr);
                f = createFunction(addr, null);
            }
            if (f == null) {
                out.println("// no function contains " + args[i] + ", and none could be made there");
                continue;
            }
            if (!done.add(f)) continue;
            out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " (asked for " + args[i] + ")");
            StringBuilder callers = new StringBuilder();
            for (Function c : f.getCallingFunctions(monitor)) {
                if (callers.length() > 0) callers.append(", ");
                callers.append(c.getName()).append("@").append(c.getEntryPoint());
            }
            out.println("// callers: " + callers);
            StringBuilder callees = new StringBuilder();
            for (Function c : f.getCalledFunctions(monitor)) {
                if (callees.length() > 0) callees.append(", ");
                callees.append(c.getName()).append("@").append(c.getEntryPoint());
            }
            out.println("// callees: " + callees);
            DecompileResults r = decomp.decompileFunction(f, 90, monitor);
            if (r.decompileCompleted()) {
                out.println(r.getDecompiledFunction().getC());
            } else {
                out.println("// decompile failed: " + r.getErrorMessage());
            }
            out.println();
        }
        decomp.dispose();
        out.close();
        println("DecompileFuncs: wrote " + done.size() + " functions to " + args[0]);
    }
}
