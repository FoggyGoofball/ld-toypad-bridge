// ExtractExports.java - Ghidra headless post-analysis script
// Extracts all exported function names and their offsets from a PRX

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.address.Address;

public class ExtractExports extends GhidraScript {

    @Override
    public void run() throws Exception {
        SymbolTable symTable = currentProgram.getSymbolTable();
        SymbolIterator symbols = symTable.getAllSymbols(false);
        
        println("=== EXPORTED FUNCTIONS ===");
        int count = 0;
        
        while (symbols.hasNext()) {
            Symbol sym = symbols.next();
            // Only show exported function symbols (not defaults or imports)
            if (sym.getSource() == SourceType.IMPORTED) {
                continue;
            }
            if (sym.isExternal()) {
                continue;
            }
            
            String name = sym.getName();
            Address addr = sym.getAddress();
            
            // Get function at this address
            Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (func != null) {
                long offset = addr.getOffset();
                // Adjust for image base if needed
                long imageBase = currentProgram.getImageBase().getOffset();
                long relativeOffset = offset - imageBase;
                
                println(String.format("EXPORT: %s @ 0x%08X (vaddr 0x%08X)", 
                    name, offset, relativeOffset));
                count++;
            }
        }
        
        // Also dump all labeled addresses in .text
        println("\n=== ALL LABELED ADDRESSES ===");
        SymbolIterator allSyms = symTable.getAllSymbols(false);
        while (allSyms.hasNext()) {
            Symbol sym = allSyms.next();
            if (sym.isExternal() || sym.getSource() == SourceType.IMPORTED) {
                continue;
            }
            String name = sym.getName();
            if (name.startsWith("DAT_") || name.startsWith("PTR_") || 
                name.startsWith("s_") || name.startsWith("UNK_")) {
                continue;
            }
            Address addr = sym.getAddress();
            println(String.format("  %s @ 0x%08X", name, addr.getOffset()));
        }
        
        println(String.format("\nTotal exports found: %d", count));
    }
}
