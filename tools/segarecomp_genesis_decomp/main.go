// SEG-077-T009: decompression evidence helper built against sega2asm (MIT, hansbonini/sega2asm @ 78f37b0d71d27b43b960e24c01c5437bbef55404).
// Usage: seg077decomp <rom> <candidates.json>  ->  JSON on stdout:
//   {"detected": [{"name","offset"}...], "results": [{"offset","algo","consumed","out_len"}...]}
// Only the formats whose decompressor routine signature is found in the ROM (types.Detect) are tried at each candidate offset. The
// consumed extent is the shortest input prefix that reproduces exactly the same output. Every single decode runs in a child process
// (this binary, --child) under a 512 MiB address-space limit and a 2 s time limit, so a runaway decoder can only fail itself.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"

	_ "sega2asm/compress"
	"sega2asm/types"
)

type result struct {
	Offset   int    `json:"offset"`
	Algo     string `json:"algo"`
	Consumed int    `json:"consumed"`
	OutLen   int    `json:"out_len"`
}

// child: decode rom[off:off+n] with one algorithm; prints "<out_len> <sha256>" or exits non-zero.
func child(rom []byte, off, n int, name string) {
	syscall.Setrlimit(syscall.RLIMIT_AS, &syscall.Rlimit{Cur: 512 << 20, Max: 512 << 20})
	go func() { time.Sleep(2 * time.Second); os.Exit(3) }()
	alg := types.LookupAlgorithm(name)
	if alg == nil || off < 0 || off+n > len(rom) || n <= 0 {
		os.Exit(2)
	}
	d, err := alg.Decompress(rom[off : off+n])
	if err != nil || len(d) == 0 {
		os.Exit(1)
	}
	h := sha256.Sum256(d)
	fmt.Printf("%d %s\n", len(d), hex.EncodeToString(h[:]))
}

var romPath string

func decode(name string, off, n int) (int, string, bool) {
	cmd := exec.Command(os.Args[0], "--child", romPath, strconv.Itoa(off), strconv.Itoa(n), name)
	out, err := cmd.Output()
	if err != nil {
		return 0, "", false
	}
	f := strings.Fields(string(out))
	if len(f) != 2 {
		return 0, "", false
	}
	l, _ := strconv.Atoi(f[0])
	return l, f[1], true
}

func main() {
	if len(os.Args) == 6 && os.Args[1] == "--child" {
		rom, err := os.ReadFile(os.Args[2])
		if err != nil {
			os.Exit(2)
		}
		off, _ := strconv.Atoi(os.Args[3])
		n, _ := strconv.Atoi(os.Args[4])
		child(rom, off, n, os.Args[5])
		return
	}
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: seg077decomp <rom> <candidates.json>")
		os.Exit(2)
	}
	romPath = os.Args[1]
	rom, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	raw, err := os.ReadFile(os.Args[2])
	if err != nil {
		panic(err)
	}
	var cands []int
	if err := json.Unmarshal(raw, &cands); err != nil {
		panic(err)
	}
	det := types.Detect(rom)
	names := map[string]bool{}
	for _, m := range det {
		names[m.Name] = true
	}
	algos := []string{}
	for n := range names {
		if types.LookupAlgorithm(n) != nil {
			algos = append(algos, n)
		}
	}
	sort.Strings(algos)
	sort.Ints(cands)
	res := []result{}
	for _, off := range cands {
		if off < 0 || off >= len(rom) {
			continue
		}
		for _, n := range algos {
			fullLen, fullHash, ok := decode(n, off, len(rom)-off)
			if !ok {
				continue
			}
			lo, hi := 1, len(rom)-off
			for lo < hi {
				mid := (lo + hi) / 2
				_, h, ok := decode(n, off, mid)
				if ok && h == fullHash {
					hi = mid
				} else {
					lo = mid + 1
				}
			}
			if _, h, ok := decode(n, off, lo); !ok || h != fullHash {
				continue
			}
			res = append(res, result{Offset: off, Algo: n, Consumed: lo, OutLen: fullLen})
		}
	}
	out, _ := json.Marshal(map[string]interface{}{"detected": det, "results": res})
	os.Stdout.Write(out)
}
