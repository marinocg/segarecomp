/* SEG-008-T001: test-side cross-check of the independent Z80 legal-form dataset against the pinned
 * redcode/Z80 oracle. Reads cases from stdin, one per line:
 *   <start> <nbytes> <byte>... <af> <bc> <expect_pc> <expect_t> <expect_r_low7>
 * places the bytes at logical addresses start, start+1, ... (wrapping at 0xFFFF; stack word 0x5678 at
 * SP = 0x8000), sets PC = start, runs one architectural step and checks
 * the exact resulting PC, the exact T-state count and the low 7 bits of R. Prints one line per mismatch
 * and a summary; exit 1 on any mismatch. Never linked into production. */
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<Z80.h>) && __has_include(<Z/constants/pointer.h>)
#define HAVE_ORACLE 1
#endif
#endif

#ifndef HAVE_ORACLE
int main(void) {
    printf("SKIP: pinned Z80 oracle headers unavailable\n");
    return 0;
}
#else
#include <Z80.h>

static unsigned char mem[65536];
static zuint8 rd(void *c, zuint16 a) { (void)c; return mem[a]; }
static void wr(void *c, zuint16 a, zuint8 v) { (void)c; mem[a] = v; }
static zuint8 in_(void *c, zuint16 p) { (void)c; (void)p; return 0x00; }
static void out_(void *c, zuint16 p, zuint8 v) { (void)c; (void)p; (void)v; }
static zuint8 inta(void *c, zuint16 a) { (void)c; (void)a; return 0xFF; }

int main(void) {
    unsigned start, n, b[64], cases = 0, bad = 0;
    unsigned af, bc, epc, et, er;
    while (scanf("%x %u", &start, &n) == 2) {
        if (n == 0 || n > 64) return 2;
        for (unsigned i = 0; i < n; i++) if (scanf("%x", &b[i]) != 1) return 2;
        if (scanf("%x %x %x %u %x", &af, &bc, &epc, &et, &er) != 5) return 2;
        Z80 z;
        memset(&z, 0, sizeof z);
        memset(mem, 0, sizeof mem);
        z.context = NULL;
        z.fetch_opcode = rd; z.fetch = rd; z.read = rd; z.write = wr; z.in = in_; z.out = out_;
        z.nop = rd; z.nmia = rd; z.inta = inta; z.int_fetch = inta;
        z.options = Z80_MODEL_ZILOG_NMOS;
        z80_power(&z, Z_TRUE);
        for (unsigned i = 0; i < n; i++) mem[(start + i) & 0xFFFFu] = (unsigned char)b[i];
        mem[0x8000] = 0x78; mem[0x8001] = 0x56;
        Z80_AF(z) = (zuint16)af; Z80_BC(z) = (zuint16)bc; Z80_DE(z) = 0x2000; Z80_HL(z) = 0x4000;
        Z80_IX(z) = 0x5000; Z80_IY(z) = 0x6000; Z80_SP(z) = 0x8000; Z80_PC(z) = (zuint16)start;
        z.i = 0; z.r = 0; z.im = 1; z.iff1 = 0; z.iff2 = 0; z.request = 0; z.resume = 0;
        z.data.uint8_array[0] = 0;
        unsigned t = (unsigned)z80_run(&z, 1);
        while (z.resume == Z80_RESUME_XY) t += (unsigned)z80_run(&z, 1);
        unsigned pc = Z80_PC(z), r = z.r & 0x7F;
        int ok = t == et && r == er && pc == epc;
        cases++;
        if (!ok) {
            bad++;
            printf("MISMATCH @%04X", start);
            for (unsigned i = 0; i < n && i < 8; i++) printf(" %02X", b[i]);
            printf(" : AF=%04X BC=%04X T=%u (want %u) R=%02X (want %02X) PC=%04X (want %04X)\n", af, bc, t, et, r, er,
                   pc, epc);
        }
    }
    printf("crosscheck: %u cases, %u mismatches\n", cases, bad);
    return bad ? 1 : 0;
}
#endif
