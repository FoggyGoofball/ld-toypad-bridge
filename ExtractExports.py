# ExtractExports.py - Ghidra headless post-analysis script
# Extracts all exported function names and their offsets from a PS3 PRX

import sys

def run():
    """Extract exports from the analyzed program."""
    program = getCurrentProgram()
    sym_table = program.getSymbolTable()
    func_mgr = program.getFunctionManager()
    image_base = program.getImageBase().getOffset()
    
    print("=== EXPORTED FUNCTIONS ===")
    count = 0
    
    # Get all symbols
    symbols = sym_table.getAllSymbols(False)
    for sym in symbols:
        if sym.isExternal():
            continue
        
        name = sym.getName()
        addr = sym.getAddress()
        
        # Skip auto-generated labels
        if name.startswith('DAT_') or name.startswith('PTR_') or name.startswith('LAB_'):
            continue
        if name.startswith('FUN_') or name.startswith('UNK_') or name.startswith('switch'):
            continue
        
        func = func_mgr.getFunctionAt(addr)
        if func is not None:
            offset = addr.getOffset()
            rel_offset = offset - image_base
            
            # Only show functions in .text range (0x0000-0x9800)
            if 0 <= rel_offset <= 0x9800:
                print("EXPORT: {} @ offset 0x{:08X}".format(name, rel_offset))
                count += 1
    
    # Also dump ALL non-auto symbols
    print("\n=== ALL LABELED ADDRESSES ===")
    for sym in symbols:
        if sym.isExternal():
            continue
        name = sym.getName()
        if name.startswith('DAT_') or name.startswith('PTR_') or name.startswith('LAB_'):
            continue
        if name.startswith('FUN_') or name.startswith('UNK_'):
            continue
        
        addr = sym.getAddress()
        offset = addr.getOffset()
        rel_offset = offset - image_base
        func = func_mgr.getFunctionAt(addr)
        ftype = "FUNC" if func else "DATA"
        print("  {} @ 0x{:08X} (rel 0x{:08X}) [{}]".format(name, offset, rel_offset, ftype))
    
    print("\nTotal cellUsbd exports: {}".format(count))

if __name__ == '__main__':
    run()
