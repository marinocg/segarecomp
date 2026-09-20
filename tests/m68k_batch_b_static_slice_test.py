#!/usr/bin/env python3
"""Synthetic B1 CMP/CMPI/CMPA generated-C checks."""
import json, subprocess, sys, tempfile
from pathlib import Path
def main():
    harness, cc = sys.argv[1:]
    vectors = [("b200", "00000001", "00000000", "00000102", "0019"), ("b200", "00000001", "00000000", "00000102", "0009"), ("b200", "00000000", "00000000", "00000102", "0014"), ("0c400080", "00000000", "00000000", "00000104", "0019"), ("b2c0", "0000FFFF", "00000000", "00000102", "0011"),
               ("0c39001200ff0000", "00000000", "00000000", "00000108", "0014"), ("0c79000000ff0000", "00000000", "00000000", "00000108", "0010"), ("0cb90000000000ff0000", "00000000", "00000000", "0000010A", "0010"),
                 ("9200", "00000001", "00000000", "00000102", "0019"), ("04400001", "00000002", "00000000", "00000104", "0000"), ("5380", "00000002", "00000000", "00000102", "0000"),
                 ("d200", "00000001", "00000000", "00000102", "0000"), ("d200", "00000001", "000000ff", "00000102", "0015"), ("d200", "00000080", "0000007f", "00000102", "0008"), ("06400001", "00000002", "00000000", "00000104", "0000"), ("5000", "00000001", "00000000", "00000102", "0000"), ("d2c0", "0000ffff", "00000000", "00000102", "0010")]
    with tempfile.TemporaryDirectory() as td:
      # Every literal Batch-B mnemonic/size form has a Dn-only synthetic
      # representative. Translation is byte-identical when repeated; this is
      # deliberately an artifact check, independent of generated execution.
      audit_codes = ("b200","b240","b280","0c010000","0c410000","0c8100000000","b2c0","b3c0",
                     "d200","d240","d280","d2c0","d3c0","06010000","06410000","068100000000","5000","5040","5080",
                     "9200","9240","9280","92c0","93c0","04010000","04410000","048100000000","5100","5140","5180",
                     "c200","c240","c280","02010000","02410000","028100000000","8200","8240","8280","00010000","00410000","008100000000",
                     "b100","b140","b180","0a010000","0a410000","0a8100000000")
      base_args = ["00000100","0010",*( ["0"] * 16 ),"0"]
      for index, code in enumerate(audit_codes):
        image = Path(td)/f"deterministic-{index}.bin"; image.write_bytes(bytes.fromhex(code))
        first = subprocess.run([harness,str(image),*base_args],text=True,capture_output=True)
        second = subprocess.run([harness,str(image),*base_args],text=True,capture_output=True)
        assert first.returncode == 0 and second.returncode == 0 and first.stdout == second.stdout, (code, first.stderr, second.stderr)
      for i,(code,d0,d1,pc,sr) in enumerate(vectors):
        image=Path(td)/f"{i}.bin"; image.write_bytes(bytes.fromhex(code))
        initial_sr = "0000" if i == 1 else "0010"
        args=[harness,str(image),"00000100",initial_sr,d0,d1,"0","0","0","0","0","0","0",d1,"0","0","0","0","0","0","0"]
        generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
        c=Path(td)/f"{i}.c"; exe=Path(td)/f"{i}"; c.write_text(generated.stdout)
        compiled=subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True); assert compiled.returncode==0, compiled.stderr
        result=subprocess.run([str(exe)],text=True,capture_output=True); assert result.returncode==0
        got=json.loads(result.stdout); assert got["pc"]==pc and got["sr"]==sr, (code,got)
      # A runtime memory destination must generate a real read/modify/write,
      # not merely a register subtraction.  Seed A0 and RAM through the same
      # B2 harness argument contract used by the differential vectors.
      image=Path(td)/"rmw.bin"; image.write_bytes(bytes.fromhex("9190"))
      args=[harness,str(image),"00000100","0010", "00000001", "0", "0", "0", "0", "0", "0", "0", "00FF0000", "0", "0", "0", "0", "0", "0", "0", "0", "00FF0000", "00000002"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      assert "ram[m68k_ea_addr_" in generated.stdout and "sub_result" in generated.stdout
      c=Path(td)/"rmw.c"; exe=Path(td)/"rmw"; c.write_text(generated.stdout)
      compiled=subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True); assert compiled.returncode==0, compiled.stderr
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["ram"] == ["00000001"], got
      # ADD uses the same destination RMW route, but must set both X and C.
      image=Path(td)/"add-rmw.bin"; image.write_bytes(bytes.fromhex("d190"))
      args=[harness,str(image),"00000100","0000", "00000001", *( ["0"] * 7 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","FFFFFFFF"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"add-rmw.c"; exe=Path(td)/"add-rmw"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["ram"] == ["00000000"] and got["sr"] == "0015", got
      # ADD.B An,Dn is illegal, but ordinary ADD.W/L An,Dn is legal and is
      # not silently reclassified as ADDA.
      for code in ("d208",):
        image=Path(td)/(code+".bin"); image.write_bytes(bytes.fromhex(code))
        rejected=subprocess.run([harness,str(image),"00000100","0000",*( ["0"] * 16 ),"0"],text=True,capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)
      for code in ("d248", "d288"):
        image=Path(td)/(code+".bin"); image.write_bytes(bytes.fromhex(code))
        args=[harness,str(image),"00000100","0010", "00000000", "00000002", *( ["0"] * 6 ),"00000001",*( ["0"] * 7 ),"0"]
        generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
        c=Path(td)/(code+".c"); exe=Path(td)/code; c.write_text(generated.stdout)
        assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
        got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
        assert got["d"][1] == "00000003" and got["a"][0] == "00000001" and got["sr"] == "0000", (code, got)
      # ADDA.L materializes its source before writing its aliased destination.
      image=Path(td)/"adda-alias.bin"; image.write_bytes(bytes.fromhex("d1d8"))
      args=[harness,str(image),"00000100","0010",*( ["0"] * 8 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","00FF0004"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"adda-alias.c"; exe=Path(td)/"adda-alias"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["a"][0] == "01FE0008" and got["sr"] == "0010", got
      # Source postincrement completes before destination evaluation.  CMPA
      # therefore compares the loaded old A0 location against incremented A0.
      image=Path(td)/"cmpa-alias.bin"; image.write_bytes(bytes.fromhex("b1d8"))
      args=[harness,str(image),"00000100","0010",*( ["0"] * 8 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","00FF0004"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"cmpa-alias.c"; exe=Path(td)/"cmpa-alias"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["a"][0] == "00FF0004" and got["sr"] == "0014", got
      # MOVEA overwrites its postincremented source An with the loaded value.
      image=Path(td)/"movea-alias.bin"; image.write_bytes(bytes.fromhex("2058"))
      args=[harness,str(image),"00000100","0010",*( ["0"] * 8 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","11223344"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"movea-alias.c"; exe=Path(td)/"movea-alias"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["a"][0] == "11223344" and got["sr"] == "0010", got
      # MOVE evaluates its destination after the source postincrement.
      image=Path(td)/"move-alias.bin"; image.write_bytes(bytes.fromhex("2098"))
      args=[harness,str(image),"00000100","0000",*( ["0"] * 8 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","11223344","00FF0004","00000000"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"move-alias.c"; exe=Path(td)/"move-alias"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["a"][0] == "00FF0004" and got["ram"] == ["11223344", "11223344"], got
      # Destination RMW postincrement remains after its write, while
      # predecrement occurs before its read.
      for code, initial_a0, seed_address, expected_a0 in (("9198", "00FF0000", "00FF0000", "00FF0004"), ("91a0", "00FF0008", "00FF0004", "00FF0004")):
        image=Path(td)/(code+".bin"); image.write_bytes(bytes.fromhex(code))
        args=[harness,str(image),"00000100","0010","00000001",*( ["0"] * 7 ),initial_a0,*( ["0"] * 7 ),"0",seed_address,"00000002"]
        generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
        c=Path(td)/(code+".c"); exe=Path(td)/code; c.write_text(generated.stdout)
        assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
        got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
        assert got["a"][0] == expected_a0 and got["ram"] == ["00000001"] and got["pc"] == "00000102" and got["sr"] == "0000", (code, got)
      # Static ROM destinations must fail during shared write resolution: no
      # generated C may index the synthetic RAM for a ROM address.
      image=Path(td)/"rom-rmw.bin"; image.write_bytes(bytes.fromhex("04B90000000100000000"))
      rejected=subprocess.run([harness,str(image),"00000100","0010",*( ["0"] * 16 ),"0"],text=True,capture_output=True)
      assert rejected.returncode == 1 and rejected.stdout == "", rejected
      # B4 logical B/W/L ordinary forms, including memory RMW and alias-safe
      # postincrement destination behavior.
      for code, d0, d1, expected_d1, expected_sr in (("c200", "000000F0", "0000000F", "00000000", "0014"),
                                                       ("8240", "0000F000", "00000F00", "0000FF00", "0018"),
                                                       ("b181", "0F0F0F0F", "00FF00FF", "0FF00FF0", "0010"),
                                                       ("020100F0", "0", "000000FF", "000000F0", "0018"),
                                                       ("004100F0", "0", "0000000F", "000000FF", "0010"),
                                                       ("0a81000000ff", "0", "000000FF", "00000000", "0014")):
        image=Path(td)/(code+".bin"); image.write_bytes(bytes.fromhex(code))
        args=[harness,str(image),"00000100","0010",d0,d1,*( ["0"] * 14 ),"0"]
        generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
        c=Path(td)/(code+".c"); exe=Path(td)/code; c.write_text(generated.stdout)
        assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
        got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
        assert got["d"][1] == expected_d1 and got["sr"] == expected_sr, (code, got)
      image=Path(td)/"logical-rmw.bin"; image.write_bytes(bytes.fromhex("c198"))
      args=[harness,str(image),"00000100","0010","0000000F",*( ["0"] * 7 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","000000F0"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      assert "logical_result" in generated.stdout and "ram[m68k_ea_addr_" in generated.stdout
      c=Path(td)/"logical-rmw.c"; exe=Path(td)/"logical-rmw"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["ram"] == ["00000000"] and got["a"][0] == "00FF0004", got
      # Logical RMW predecrement materializes the decremented address once:
      # the neighbor remains untouched, A0 retains the decremented address,
      # and the result/CCR use the same shared logical specification as B/W/L
      # register forms above.
      image=Path(td)/"logical-predec.bin"; image.write_bytes(bytes.fromhex("c1a0"))
      args=[harness,str(image),"00000100","0013","0000000F",*( ["0"] * 7 ),"00FF0008",*( ["0"] * 7 ),"0","00FF0000","11223344","00FF0004","F0F0F0F0"]
      generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, generated.stderr
      c=Path(td)/"logical-predec.c"; exe=Path(td)/"logical-predec"; c.write_text(generated.stdout)
      assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
      got=json.loads(subprocess.run([str(exe)],text=True,capture_output=True).stdout)
      assert got["a"][0] == "00FF0004" and got["ram"] == ["11223344", "00000000"] and got["sr"] == "0014", got
      for code in ("003c", "007c", "023c", "027c", "0a3c", "0a7c", "c108", "b108"):
        image=Path(td)/(code+".bin"); image.write_bytes(bytes.fromhex(code))
        rejected=subprocess.run([harness,str(image),"00000100","0000",*( ["0"] * 16 ),"0"],text=True,capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", (code, rejected)
      # Every B4 direction/size reaches the same static harness route. Memory
      # forms get a valid A0/RAM seed; zero operands make their result stable.
      for code in ("c200","c240","c280","c110","c150","c190",
                   "8200","8240","8280","8110","8150","8190",
                   "b101","b141","b181","b110","b150","b190",
                   "02010000","02410000","028100000000",
                   "00010000","00410000","0a8100000000"):
        image=Path(td)/(code+"-all.bin"); image.write_bytes(bytes.fromhex(code))
        args=[harness,str(image),"00000100","0010",*( ["0"] * 8 ),"00FF0000",*( ["0"] * 7 ),"0","00FF0000","00000000"]
        generated=subprocess.run(args,text=True,capture_output=True); assert generated.returncode==0, (code,generated.stderr)
        c=Path(td)/(code+"-all.c"); exe=Path(td)/(code+"-all"); c.write_text(generated.stdout)
        assert subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-pedantic",str(c),"-o",str(exe)],text=True,capture_output=True).returncode==0
    print("validated synthetic Batch B generated-C vectors and deterministic B1--B4 whitelist audit")
if __name__ == '__main__': main()
