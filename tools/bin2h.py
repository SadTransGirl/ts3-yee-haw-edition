"""Write a binary file as a C byte array: bin2h.py <file> <name> > out.h"""
import sys

data = open(sys.argv[1], 'rb').read()
name = sys.argv[2]
print(f"// Generated from {sys.argv[1].replace(chr(92), '/').split('/')[-1]} by re/tools/bin2h.py. Don't edit.")
print(f"static const unsigned char {name}[{len(data)}] = {{")
for i in range(0, len(data), 20):
    print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
print("};")
