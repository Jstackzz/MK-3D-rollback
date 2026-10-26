// Apply names to the current program from text files whose lines look like
//   NAME = 0xADDRESS            (XboxXBERecomp game.toml [symbols]/[data])
//   LIB__NAME = 0xADDRESS       (XbSymbolDatabaseCLI output; LIB__ is dropped)
// Functions at the address are renamed, anything else gets a primary label.
// Headless usage: -postScript ApplyNames.java <file1> [<file2> ...]
// @category MK3D
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.DuplicateNameException;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.HashSet;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        Pattern line = Pattern.compile("^\\s*([A-Za-z0-9_]+)\\s*=\\s*0x([0-9A-Fa-f]+)");
        Set<String> used = new HashSet<>();
        int renamed = 0, labelled = 0, skipped = 0;
        for (String file : getScriptArgs()) {
            try (BufferedReader r = new BufferedReader(new FileReader(file))) {
                String s;
                while ((s = r.readLine()) != null) {
                    Matcher m = line.matcher(s);
                    if (!m.find()) continue;
                    String name = m.group(1);
                    int sep = name.indexOf("__");
                    if (sep > 0 && sep < 12 && name.substring(0, sep).equals(name.substring(0, sep).toUpperCase())) {
                        name = name.substring(sep + 2);
                    }
                    long value = Long.parseLong(m.group(2), 16);
                    if (value == 0) continue;
                    Address addr = toAddr(value);
                    if (!currentProgram.getMemory().contains(addr)) { skipped++; continue; }
                    String unique = name;
                    int n = 2;
                    while (used.contains(unique)) unique = name + "_" + (n++);
                    Function f = getFunctionAt(addr);
                    try {
                        if (f != null) {
                            f.setName(unique, SourceType.USER_DEFINED);
                            renamed++;
                        } else {
                            createLabel(addr, unique, true, SourceType.USER_DEFINED);
                            labelled++;
                        }
                        used.add(unique);
                    } catch (DuplicateNameException e) {
                        skipped++;
                    }
                }
            }
        }
        println("ApplyNames: renamed " + renamed + " functions, " + labelled + " labels, skipped " + skipped);
    }
}
