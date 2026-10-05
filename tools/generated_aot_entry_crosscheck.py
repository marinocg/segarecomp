#!/usr/bin/env python3
"""SEG-036 (ADR 0083): exhaustive cross-check of the direct-entry representation against the legacy owner/wrapper form.

Usage: generated_aot_entry_crosscheck.py <legacy-shard-dir> <direct-shard-dir>
(both emitted from the same image: the second without and the first with `--legacy-aot-entries`, i.e. pass the legacy
directory first). For every helper-backed compiled PC it proves that the direct tables select the same shared helper, rebuild the
same provenance fields the legacy wrapper spelled, and that an own-PC argument was that PC; that no owner-backed PC changed
representation; and that every shared helper body is identical (ignoring the uniform-signature `(void)` lines). Prints aggregates only.
"""
leg, dr = sys.argv[1:3]
# legacy wrappers: pc -> (helper, prov tuple or None, pcarg or None)
call=re.compile(rb"^genesis_aot_entry_([0-9A-F]{8}): \{ return (genesis_aot_shared_\d+)\(runtime(?:, &\(const GenesisInstructionProvenance\)\{GENESIS_CPU_MC68000, UINT32_C\(0x([0-9A-F]{8})\), UINT64_C\((\d+)\), \{UINT8_C\(0x([0-9A-F]{2})\), UINT8_C\(0x([0-9A-F]{2})\)\}, UINT32_C\((\d+)\)\})?(?:, UINT32_C\(0x([0-9A-F]{8})\))?\); \}\n$")
L={}; inline=set()
for f in sorted(glob.glob(leg+"/bridge_generated_aot_*.c")):
    for line in open(f,"rb"):
        m=call.match(line)
        if m:
            pc=int(m.group(1),16); prov=None
            if m.group(3): prov=(int(m.group(3),16),int(m.group(4)),int(m.group(5),16),int(m.group(6),16),int(m.group(7)))
            L[pc]=(m.group(2).decode(),prov,int(m.group(8),16) if m.group(8) else None)
        elif line.startswith(b"genesis_aot_entry_"): inline.add(int(line[18:26],16))
# direct tables
txt=b"".join(open(f,"rb").read() for f in glob.glob(dr+"/bridge_generated_entries_*.c")).decode()
def arr(n): return re.search(r"\b"+n+r"\[\] = \{\n(.*?)\n\};",txt,re.S).group(1).splitlines()
addr=[int(x.strip()[2:10],16) for x in arr("genesis_compiled_entry_addresses")]
ids=[int(x.strip().rstrip(",")) for x in arr("genesis_compiled_entry_owner_ids")]
owners=[x.strip().rstrip(",") for x in arr("genesis_compiled_owners")]
helpers=[x.strip().rstrip(",") for x in arr("genesis_aot_direct_helpers")]
meta=[x.strip().rstrip(",") for x in arr("genesis_aot_direct_meta")]
assert len(addr)==len(ids)==len(meta)
bad=0; direct=0
for pc,i,m in zip(addr,ids,meta):
    if i<len(owners): 
        assert pc not in L, hex(pc); continue
    direct+=1
    h=helpers[i-len(owners)]
    exp=L[pc]
    assert exp[0]==h,(hex(pc),exp[0],h)
    w=int(m[:-3],16) if m!="0ULL" else 0
    if exp[1] is None:
        assert w==0,(hex(pc),"no-source helper has word")
    else:
        got=(pc, w>>24, (w>>8)&0xFF,(w>>16)&0xFF, w&0xFF)
        assert got==exp[1],(hex(pc),got,exp[1])
    if exp[2] is not None: assert exp[2]==pc
assert direct==len(L), (direct,len(L))
print("direct entries",direct,"legacy wrappers",len(L),"inline legacy",len(inline),"owner-backed in direct build",len(addr)-direct)
# helper bodies identical modulo signature
def bodies(d):
    out={}
    for f in glob.glob(d+"/bridge_generated_shared_*.c"):
        t=open(f,"rb").read().decode()
        for m in re.finditer(r"^(?:static )?GenesisControlTransfer (genesis_aot_shared_\d+)\(([^)]*)\) \{\n(.*?)^\}\n",t,re.S|re.M):
            body=re.sub(r"^  \(void\)genesis_aot_(?:source|pc);\n","",m.group(3),flags=re.M)
            out[m.group(1)]=hashlib.sha256(body.encode()).hexdigest()
    return out
a,b=bodies(leg),bodies(dr)
assert a==b and len(a)>1000,(len(a),len(b))
print("helper bodies identical:",len(a))
