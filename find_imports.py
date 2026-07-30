"""
find_imports.py — Find all .sprx module references in EBOOT.elf
"""
import re

with open(r"c:\Users\Admin\source\repos\dimensions plugin\EBOOT.elf", 'rb') as f:
    data = f.read()

# Find all .sprx references
pattern = re.compile(rb'([a-zA-Z0-9_/]+\.sprx)')
matches = set()
for m in pattern.finditer(data):
    name = m.group(1).decode('ascii', errors='ignore')
    matches.add(name)

print(f"Found {len(matches)} unique .sprx imports:")
for name in sorted(matches):
    print(f"  {name}")
