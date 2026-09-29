/* SEG-008-T001: test-side cross-check of the independent Z80 legal-form dataset against the pinned
 * redcode/Z80 oracle. Reads cases from stdin, one per line:
 *   <nbytes> <byte>... <expect_len|-1> <t_a> <t_b> <expect_r_increment> <bc>
 * places the bytes at 0x1000, runs one architectural step and checks the T-state count is t_a or t_b,
 * the low-7-bit R increment, and (when expect_len >= 0) PC advance == length. Prints one line per
 * mismatch and a summary; exit 1 on any mismatch. Never linked into production. */
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
    unsigned n, b[64], cases = 0, bad = 0;
    int expect_len;
    unsigned ta, tb, er, bc;
    while (scanf("%u", &n) == 1) {
        if (n == 0 || n > 64) return 2;
        for (unsigned i = 0; i < n; i++) if (scanf("%x", &b[i]) != 1) return 2;
        if (scanf("%d %u %u %u %x", &expect_len, &ta, &tb, &er, &bc) != 5) return 2;
        Z80 z;
        memset(&z, 0, sizeof z);
        memset(mem, 0, sizeof mem);
        z.context = NULL;
        z.fetch_opcode = rd; z.fetch = rd; z.read = rd; z.write = wr; z.in = in_; z.out = out_;
        z.nop = rd; z.nmia = rd; z.inta = inta; z.int_fetch = inta;
        z.options = Z80_MODEL_ZILOG_NMOS;
        z80_power(&z, Z_TRUE);
        for (unsigned i = 0; i < n; i++) mem[0x1000 + i] = (unsigned char)b[i];
        Z80_AF(z) = 0x0000; Z80_BC(z) = (zuint16)bc; Z80_DE(z) = 0x2000; Z80_HL(z) = 0x4000;
        Z80_IX(z) = 0x5000; Z80_IY(z) = 0x6000; Z80_SP(z) = 0x8000; Z80_PC(z) = 0x1000;
        z.i = 0; z.r = 0; z.im = 1; z.iff1 = 0; z.iff2 = 0; z.request = 0; z.resume = 0;
        z.data.uint8_array[0] = 0;
        unsigned t = (unsigned)z80_run(&z, 1);
        while (z.resume == Z80_RESUME_XY) t += (unsigned)z80_run(&z, 1);
        unsigned pc = Z80_PC(z), r = z.r & 0x7F;
        int ok = (t == ta || t == tb) && r == er && (expect_len < 0 || pc == 0x1000u + (unsigned)expect_len);
        cases++;
        if (!ok) {
            bad++;
            printf("MISMATCH");
            for (unsigned i = 0; i < n && i < 8; i++) printf(" %02X", b[i]);
            printf(" : T=%u (want %u/%u) R=%u (want %u) PCdelta=%u (want %d)\n", t, ta, tb, r, er, pc - 0x1000u,
                   expect_len);
        }
    }
    printf("crosscheck: %u cases, %u mismatches\n", cases, bad);
    return bad ? 1 : 0;
}
#endif
