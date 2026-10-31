#!/usr/bin/env python3
"""Lists each distinct valgrind memcheck report in the logs given once, with the first frame
outside valgrind and libc, the scenarios it showed in, and where an uninitialised value came
from. Mismatched new/delete reports are left out (Borland's heap and ours treat them alike).
    tools/valgrind-summary.py <log>...
"""
import sys,re,collections,os
c=collections.Counter(); where=collections.defaultdict(set); origin={}
for f in sys.argv[1:]:
    name=os.path.basename(f)[:-3]
    block=[]
    def flush(block):
        if not block: return
        msg=block[0]
        if msg.startswith('Mismatched') or not re.match(r'(Conditional|Use of|Syscall|Invalid|Source)', msg): return
        frames=[l for l in block[1:] if re.match(r'\s*(at|by) 0x', l)]
        loc=next((re.sub(r'^\s*(at|by) 0x[0-9A-F]+: ','',l) for l in frames if 'vg_replace' not in l and '(in /usr' not in l), '?')
        org=next((re.sub(r'^\s*(at|by) 0x[0-9A-F]+: ','',l) for i,l in enumerate(block) if 'Uninitialised value was created' in l for l in block[i+1:i+4] if 'vg_replace' not in l and re.match(r'\s*(at|by) 0x',l)), '')
        key=(msg[:50],loc[:90]); c[key]+=1; where[key].add(name); origin.setdefault(key,org[:80])
    for line in open(f,errors='replace'):
        m=re.match(r'==\d+== ?(.*)$', line.rstrip('\n'))
        if not m: continue
        t=m.group(1)
        if t=='': flush(block); block=[]
        else: block.append(t)
    flush(block)
for (k,l),n in sorted(c.items(), key=lambda x:(x[0][1])):
    print(f"{k:50} | {l:90} | {','.join(sorted(where[(k,l)]))} | from {origin[(k,l)]}")
