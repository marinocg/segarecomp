/* Minimal Z80 oracle adapter smoke (SEG-008-T001): pinned redcode/Z80 (+ redcode/Zeta headers).
 *
 * Sets a full CPU state, runs exactly one instruction (or one interrupt response), records every
 * memory / I/O / interrupt-acknowledge callback (kind, address, value, direction) and reads the
 * full state back, including MEMPTR (WZ), Q, IFF1/IFF2, IM, R, HALT and the T-state count.
 *
 * Build (checkout root = directory holding the pinned redcode_Z80 and redcode_Zeta clones):
 *   O=$SEGARECOMP_Z80_ORACLE_CHECKOUT
 *   cc -std=c11 -Wall -Wextra -DZ80_STATIC -DZ80_WITH_EXECUTE -DZ80_WITH_Q -DZ80_WITH_FULL_IM0 \
 *      -DZ80_WITH_SPECIAL_RESET -DZ80_WITH_UNOFFICIAL_RETI -DZ80_WITH_ZILOG_NMOS_LD_A_IR_BUG \
 *      -I"$O/redcode_Z80/API" -I"$O/redcode_Zeta/API" \
 *      adapter_smoke.c "$O/redcode_Z80/sources/Z80.c" -o adapter_smoke && ./adapter_smoke
 * Without the include paths the program still compiles and prints a SKIP line (exit 0); a driver
 * test additionally skips when SEGARECOMP_Z80_ORACLE_CHECKOUT is unset and fails on a wrong pin.
 */
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<Z80.h>) && __has_include(<Z/constants/pointer.h>)
#define HAVE_ORACLE 1
#endif
#endif

#ifndef HAVE_ORACLE
int main(void) {
    printf("SKIP: pinned Z80 oracle headers unavailable (set SEGARECOMP_Z80_ORACLE_CHECKOUT)\n");
    return 0;
}
#else
#include <Z80.h>

typedef struct { char kind; unsigned addr, value; } bus_event; /* F=M1 fetch R=read W=write I=in O=out A=INTA */
typedef struct {
    unsigned char mem[65536];
    bus_event ev[64];
    int nev;
    unsigned char port_in, int_data;
} bus_t;

static void logev(bus_t *b, char k, unsigned a, unsigned v) {
    if (b->nev < 64) { b->ev[b->nev].kind = k; b->ev[b->nev].addr = a; b->ev[b->nev].value = v; b->nev++; }
}
static zuint8 cb_fetch_opcode(void *c, zuint16 a) { bus_t *b = c; logev(b, 'F', a, b->mem[a]); return b->mem[a]; }
static zuint8 cb_read(void *c, zuint16 a) { bus_t *b = c; logev(b, 'R', a, b->mem[a]); return b->mem[a]; }
static void cb_write(void *c, zuint16 a, zuint8 v) { bus_t *b = c; b->mem[a] = v; logev(b, 'W', a, v); }
static zuint8 cb_in(void *c, zuint16 p) { bus_t *b = c; logev(b, 'I', p, b->port_in); return b->port_in; }
static void cb_out(void *c, zuint16 p, zuint8 v) { bus_t *b = c; logev(b, 'O', p, v); }
static zuint8 cb_nop(void *c, zuint16 a) { bus_t *b = c; logev(b, 'F', a, b->mem[a]); return b->mem[a]; }
static zuint8 cb_nmia(void *c, zuint16 a) { bus_t *b = c; logev(b, 'F', a, b->mem[a]); return b->mem[a]; }
static zuint8 cb_inta(void *c, zuint16 a) { bus_t *b = c; (void)a; logev(b, 'A', 0, b->int_data); return b->int_data; }

typedef struct {
    unsigned af, bc, de, hl, af_, bc_, de_, hl_, ix, iy, sp, pc, wz;
    unsigned i, r, im, iff1, iff2, q, halted, ei_pending;
} state_t;

static void init_cpu(Z80 *z, bus_t *b) {
    memset(z, 0, sizeof *z);
    z->context = b;
    z->fetch_opcode = cb_fetch_opcode; z->fetch = cb_read; z->read = cb_read; z->write = cb_write;
    z->in = cb_in; z->out = cb_out; z->nop = cb_nop; z->nmia = cb_nmia; z->inta = cb_inta;
    z->int_fetch = cb_inta;
    z->options = Z80_MODEL_ZILOG_NMOS;
    z80_power(z, Z_TRUE);
}
static void set_state(Z80 *z, const state_t *s) {
    Z80_AF(*z) = (zuint16)s->af; Z80_BC(*z) = (zuint16)s->bc; Z80_DE(*z) = (zuint16)s->de; Z80_HL(*z) = (zuint16)s->hl;
    Z80_AF_(*z) = (zuint16)s->af_; Z80_BC_(*z) = (zuint16)s->bc_; Z80_DE_(*z) = (zuint16)s->de_; Z80_HL_(*z) = (zuint16)s->hl_;
    Z80_IX(*z) = (zuint16)s->ix; Z80_IY(*z) = (zuint16)s->iy; Z80_SP(*z) = (zuint16)s->sp; Z80_PC(*z) = (zuint16)s->pc;
    Z80_MEMPTR(*z) = (zuint16)s->wz;
    z->i = (zuint8)s->i; z->r = (zuint8)s->r; z->im = (zuint8)s->im;
    z->iff1 = (zuint8)s->iff1; z->iff2 = (zuint8)s->iff2; z->q = (zuint8)s->q;
    z->halt_line = (zuint8)(s->halted != 0);
    z->resume = s->halted ? Z80_RESUME_HALT : 0;
    z->data.uint8_array[0] = s->ei_pending ? 0xFB : 0x00; /* the core gates INT on "previous opcode was EI" */
    z->request = 0;
}
static void get_state(const Z80 *z, state_t *s) {
    s->af = Z80_AF(*z); s->bc = Z80_BC(*z); s->de = Z80_DE(*z); s->hl = Z80_HL(*z);
    s->af_ = Z80_AF_(*z); s->bc_ = Z80_BC_(*z); s->de_ = Z80_DE_(*z); s->hl_ = Z80_HL_(*z);
    s->ix = Z80_IX(*z); s->iy = Z80_IY(*z); s->sp = Z80_SP(*z); s->pc = Z80_PC(*z); s->wz = Z80_MEMPTR(*z);
    s->i = z->i; s->r = z->r; s->im = z->im; s->iff1 = z->iff1; s->iff2 = z->iff2; s->q = z->q;
    s->halted = (z->halt_line || z->resume == Z80_RESUME_HALT);
    s->ei_pending = (z->data.uint8_array[0] == 0xFB);
}
/* One architectural step: an instruction, or an interrupt response if one is pending and
 * acceptable. A DD/FD prefix run is completed (the core yields after each prefix when the cycle
 * budget is exhausted). */
static unsigned step_one(Z80 *z) {
    unsigned t = (unsigned)z80_run(z, 1);
    while (z->resume == Z80_RESUME_XY) t += (unsigned)z80_run(z, 1);
    return t;
}
static void dump(const char *title, const state_t *s, unsigned t, const bus_t *b) {
    printf("%s: T=%u PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X WZ=%04X "
           "I=%02X R=%02X IM=%u IFF1=%u IFF2=%u Q=%02X HALT=%u EIPEND=%u\n",
           title, t, s->pc, s->sp, s->af, s->bc, s->de, s->hl, s->ix, s->iy, s->wz,
           s->i, s->r, s->im, s->iff1, s->iff2, s->q, s->halted, s->ei_pending);
    for (int i = 0; i < b->nev; i++)
        printf("  bus %c %04X %02X %s\n", b->ev[i].kind, b->ev[i].addr, b->ev[i].value,
               (b->ev[i].kind == 'W' || b->ev[i].kind == 'O') ? "out" : "in");
}

static int failures;
static void expect(const char *what, unsigned got, unsigned want) {
    if (got != want) { printf("FAIL %s: got %X want %X\n", what, got, want); failures++; }
}

int main(void) {
    static bus_t bus;
    Z80 cpu;
    state_t s, o;
    unsigned t;

    /* 1. INI (ED A2): two M1 fetches, one port read at BC (before B decrements), one memory write. */
    memset(&bus, 0, sizeof bus);
    init_cpu(&cpu, &bus);
    memset(&s, 0, sizeof s);
    s.af = 0x1200; s.bc = 0x0210; s.de = 0x3344; s.hl = 0x4000; s.af_ = 0xAAAA; s.bc_ = 0xBBBB;
    s.de_ = 0xCCCC; s.hl_ = 0xDDDD; s.ix = 0x1111; s.iy = 0x2222; s.sp = 0x8000; s.pc = 0x1000;
    s.wz = 0x5555; s.i = 0x3F; s.r = 0x7F; s.im = 1; s.iff1 = 1; s.iff2 = 1; s.q = 0;
    bus.mem[0x1000] = 0xED; bus.mem[0x1001] = 0xA2; bus.port_in = 0xF8;
    set_state(&cpu, &s);
    t = step_one(&cpu);
    get_state(&cpu, &o);
    dump("INI", &o, t, &bus);
    expect("INI T", t, 16); expect("INI PC", o.pc, 0x1002); expect("INI B", o.bc, 0x0110);
    expect("INI HL", o.hl, 0x4001); expect("INI mem", bus.mem[0x4000], 0xF8);
    expect("INI F", o.af & 0xFF, 0x17); expect("INI WZ", o.wz, 0x0211); expect("INI R", o.r, 0x01);
    expect("INI Q", o.q, 0x17);
    expect("INI events", (unsigned)bus.nev, 4);
    expect("INI port", bus.ev[2].kind == 'I' && bus.ev[2].addr == 0x0210, 1);

    /* 2. OUTI (ED A3): memory read at HL, port write at BC after B decrements. */
    memset(&bus, 0, sizeof bus);
    init_cpu(&cpu, &bus);
    s.af = 0x0000; s.bc = 0x0210; s.hl = 0x4000; s.r = 0x00;
    bus.mem[0x1000] = 0xED; bus.mem[0x1001] = 0xA3; bus.mem[0x4000] = 0xF8;
    set_state(&cpu, &s);
    t = step_one(&cpu);
    get_state(&cpu, &o);
    dump("OUTI", &o, t, &bus);
    expect("OUTI T", t, 16); expect("OUTI F", o.af & 0xFF, 0x06); expect("OUTI WZ", o.wz, 0x0111);
    expect("OUTI port", bus.ev[3].kind == 'O' && bus.ev[3].addr == 0x0110 && bus.ev[3].value == 0xF8, 1);

    /* 3. IM2 interrupt response: INTA supplies an odd vector; table read via ordinary reads. */
    memset(&bus, 0, sizeof bus);
    init_cpu(&cpu, &bus);
    memset(&s, 0, sizeof s);
    s.sp = 0x8000; s.pc = 0x1000; s.i = 0x80; s.im = 2; s.iff1 = 1; s.iff2 = 1;
    bus.int_data = 0x11; bus.mem[0x8011] = 0x78; bus.mem[0x8012] = 0x56;
    set_state(&cpu, &s);
    z80_int(&cpu, Z_TRUE);
    t = step_one(&cpu);
    z80_int(&cpu, Z_FALSE);
    get_state(&cpu, &o);
    dump("IM2", &o, t, &bus);
    expect("IM2 T", t, 19); expect("IM2 PC", o.pc, 0x5678); expect("IM2 WZ", o.wz, 0x5678);
    expect("IM2 IFF1", o.iff1, 0); expect("IM2 IFF2", o.iff2, 0); expect("IM2 R", o.r, 1);
    expect("IM2 ret", bus.mem[0x7FFE] | (bus.mem[0x7FFF] << 8), 0x1000);

    /* 4. HALT then NMI: PC advances past HALT, halted M1 cycles fetch HALT+1, R counts them. */
    memset(&bus, 0, sizeof bus);
    init_cpu(&cpu, &bus);
    s.im = 1; s.iff1 = 1; s.iff2 = 1; s.r = 0;
    bus.mem[0x1000] = 0x76;
    set_state(&cpu, &s);
    t = step_one(&cpu) + step_one(&cpu);
    get_state(&cpu, &o);
    dump("HALT+1", &o, t, &bus);
    expect("HALT PC", o.pc, 0x1001); expect("HALT halted", o.halted, 1); expect("HALT R", o.r, 2);
    bus.nev = 0;
    z80_nmi(&cpu);
    t = step_one(&cpu);
    get_state(&cpu, &o);
    dump("NMI", &o, t, &bus);
    expect("NMI T", t, 11); expect("NMI PC", o.pc, 0x66); expect("NMI IFF1", o.iff1, 0);
    expect("NMI IFF2", o.iff2, 1); expect("NMI ret", bus.mem[0x7FFE] | (bus.mem[0x7FFF] << 8), 0x1001);

    /* 5. Prefix lock (ADR 0058): a full 64 KiB mapping of DD bytes never reaches an opcode. Fetch wraps at
     *    0xFFFF, every prefix is 4 T and one M1 (R += 1), and once the run has started neither INT (IFF1 = 1,
     *    line asserted) nor NMI is ever accepted inside it. */
    memset(&bus, 0, sizeof bus);
    init_cpu(&cpu, &bus);
    memset(bus.mem, 0xDD, sizeof bus.mem);
    memset(&s, 0, sizeof s);
    s.sp = 0x8000; s.pc = 0xFFF0; s.im = 1; s.iff1 = 1; s.iff2 = 1; s.r = 0;
    set_state(&cpu, &s);
    t = (unsigned)z80_run(&cpu, 40);     /* enter the prefix run first: no interrupt pending yet */
    z80_int(&cpu, Z_TRUE);               /* then assert INT (IFF1 = 1) ... */
    t += (unsigned)z80_run(&cpu, 380);
    z80_nmi(&cpu);                       /* ... and raise NMI while INT stays asserted */
    t += (unsigned)z80_run(&cpu, 380);
    get_state(&cpu, &o);
    printf("PREFIX-LOCK: T=%u PC=%04X SP=%04X R=%02X IFF1=%u IFF2=%u\n", t, o.pc, o.sp, o.r, o.iff1, o.iff2);
    /* Suspended inside a prefix run the core's PC names the last fetched prefix; the contract's architectural
     * value is the next fetch address, so the adapter normalises it (as it normalises HALT). */
    expect("LOCK suspended in prefix run", cpu.resume == Z80_RESUME_XY, 1);
    expect("LOCK T", t, 800);
    expect("LOCK next-fetch PC (wrapped)", (o.pc + 1u) & 0xFFFFu, (0xFFF0u + 200u) & 0xFFFFu);
    expect("LOCK SP (no interrupt push)", o.sp, 0x8000); expect("LOCK R", o.r, 200u & 0x7Fu);
    expect("LOCK IFF1", o.iff1, 1); expect("LOCK IFF2", o.iff2, 1);

    printf(failures ? "adapter smoke: %d failure(s)\n" : "adapter smoke: OK%.0d\n", failures);
    return failures ? 1 : 0;
}
#endif
