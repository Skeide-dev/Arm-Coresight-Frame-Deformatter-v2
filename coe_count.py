import sys, re

with open(sys.argv[1]) as f:
    data = f.read()

values = re.findall(r'[\da-fA-F]+', data.split('=')[-1].replace('}', ''))
print(f"{len(values)} bytes")