/* Test-only pinned Musashi runner for SEG-007-T013 TST.L (xxx).L validation.
 *
 * The committed Python validator builds this source in a TemporaryDirectory
 * against the revision-checked, ignored Musashi checkout.  This runner shares
 * no decoder, IR, emitter, or runtime code with segarecomp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

#define RAM_BEGIN UINT32_C(0x00FF0000)
#define RAM_SIZE UINT32_C(0x00010000)
#define MAX_ACCESS_RECORDS 16U

#ifndef MUSASHI_GIT_REVISION
#error "MUSASHI_GIT_REVISION must be supplied by the committed validator"
#endif

static uint8_t *image;
static size_t image_size;
static uint8_t ram[RAM_SIZE];
static unsigned long writes;
static uint32_t executing_instruction_pc;
static unsigned int absolute_long_extension_fetch_pending;

struct access_record {
    uint32_t address;
    uint32_t value;
};

static struct access_record operand_reads[MAX_ACCESS_RECORDS];
static unsigned long operand_read_count;

static unsigned int read8(unsigned int address) {
    if ((uint64_t)address < image_size) return image[address];
    if (address >= RAM_BEGIN && address < RAM_BEGIN + RAM_SIZE) return ram[address - RAM_BEGIN];
    return 0U;
}

static void write8(unsigned int address, unsigned int value) {
    ++writes;
    if (address >= RAM_BEGIN && address < RAM_BEGIN + RAM_SIZE) ram[address - RAM_BEGIN] = (uint8_t)value;
}

static unsigned int read16_raw(unsigned int address) {
    return (read8(address) << 8U) | read8(address + 1U);
}

static unsigned int read32_raw(unsigned int address) {
    return (read16_raw(address) << 16U) | read16_raw(address + 2U);
}

static void record_operand_read(unsigned int address, unsigned int value) {
    /* Musashi reads this instruction's absolute-long extension through the
     * generic 32-bit callback at PC+2.  Suppress that one fetch only: an
     * operand read may legally overlap code, including PC or PC+2. */
    if (absolute_long_extension_fetch_pending && address == executing_instruction_pc + 2U) {
        absolute_long_extension_fetch_pending = 0U;
        return;
    }
    if (operand_read_count < MAX_ACCESS_RECORDS) {
        operand_reads[operand_read_count].address = address;
        operand_reads[operand_read_count].value = value;
    }
    ++operand_read_count;
}

unsigned int m68k_read_memory_8(unsigned int address) { return read8(address); }
unsigned int m68k_read_memory_16(unsigned int address) { return read16_raw(address); }
unsigned int m68k_read_memory_32(unsigned int address) {
    unsigned int value = read32_raw(address);
    record_operand_read(address, value);
    return value;
}
unsigned int m68k_read_immediate_16(unsigned int address) { return read16_raw(address); }
unsigned int m68k_read_immediate_32(unsigned int address) { return read32_raw(address); }
unsigned int m68k_read_pcrelative_8(unsigned int address) { return read8(address); }
unsigned int m68k_read_pcrelative_16(unsigned int address) { return read16_raw(address); }
unsigned int m68k_read_pcrelative_32(unsigned int address) { return read32_raw(address); }
unsigned int m68k_read_disassembler_8(unsigned int address) { return read8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return read16_raw(address); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return read32_raw(address); }
void m68k_write_memory_8(unsigned int address, unsigned int value) { write8(address, value); }
void m68k_write_memory_16(unsigned int address, unsigned int value) {
    write8(address, value >> 8U);
    write8(address + 1U, value);
}
void m68k_write_memory_32(unsigned int address, unsigned int value) {
    m68k_write_memory_16(address, value >> 16U);
    m68k_write_memory_16(address + 2U, value);
}
void m68k_write_memory_32_pd(unsigned int address, unsigned int value) {
    m68k_write_memory_32(address, value);
}

static uint32_t parse_hex(const char *text, size_t digits) {
    char *end = NULL;
    unsigned long value;
    if (strlen(text) != digits) {
        fprintf(stderr, "invalid hexadecimal argument\n");
        exit(2);
    }
    value = strtoul(text, &end, 16);
    if (*end != '\0' || value > UINT32_MAX) {
        fprintf(stderr, "invalid hexadecimal argument\n");
        exit(2);
    }
    return (uint32_t)value;
}

int main(int argc, char **argv) {
    uint32_t d[8];
    uint32_t pc;
    uint32_t sr;
    int index;
    FILE *handle;
    long length;

    /* runner IMAGE_PATH PC SR D0..D7 [RAM_ADDRESS RAM_VALUE]... */
    if (argc < 12 || (argc - 12) % 2 != 0) {
        fprintf(stderr, "usage: tst-l-musashi-runner IMAGE PC SR D0..D7 [RAM_ADDRESS RAM_VALUE]...\n");
        return 2;
    }
    handle = fopen(argv[1], "rb");
    if (handle == NULL) {
        fprintf(stderr, "cannot open image\n");
        return 2;
    }
    if (fseek(handle, 0, SEEK_END) != 0 || (length = ftell(handle)) < 0 || length > 0x400000L) {
        fclose(handle);
        fprintf(stderr, "unsupported image size\n");
        return 2;
    }
    rewind(handle);
    image_size = (size_t)length;
    image = calloc(image_size + 1U, 1U);
    if (image == NULL || (image_size != 0U && fread(image, 1U, image_size, handle) != image_size)) {
        fclose(handle);
        free(image);
        fprintf(stderr, "cannot read image\n");
        return 2;
    }
    fclose(handle);

    pc = parse_hex(argv[2], 8U);
    sr = parse_hex(argv[3], 4U);
    for (index = 0; index < 8; ++index) d[index] = parse_hex(argv[index + 4], 8U);
    for (index = 12; index < argc; index += 2) {
        uint32_t address = parse_hex(argv[index], 8U);
        uint32_t value = parse_hex(argv[index + 1], 8U);
        if (address < RAM_BEGIN || (uint64_t)address + 4U > (uint64_t)RAM_BEGIN + RAM_SIZE) {
            free(image);
            fprintf(stderr, "ram seed outside the synthetic work-RAM window\n");
            return 2;
        }
        ram[address - RAM_BEGIN] = (uint8_t)(value >> 24U);
        ram[address - RAM_BEGIN + 1U] = (uint8_t)(value >> 16U);
        ram[address - RAM_BEGIN + 2U] = (uint8_t)(value >> 8U);
        ram[address - RAM_BEGIN + 3U] = (uint8_t)value;
    }

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    (void)m68k_execute(132);
    for (index = 0; index < 8; ++index) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + index), d[index]);
    }
    m68k_set_reg(M68K_REG_PC, pc);
    m68k_set_reg(M68K_REG_SR, sr);
    writes = 0UL;
    operand_read_count = 0UL;
    executing_instruction_pc = pc;
    absolute_long_extension_fetch_pending = 1U;
    /* A one-cycle budget makes Musashi retire exactly one selected instruction. */
    (void)m68k_execute(1);

    if (operand_read_count > MAX_ACCESS_RECORDS) {
        free(image);
        fprintf(stderr, "too many operand read records\n");
        return 2;
    }
    printf("{\"musashi_revision\":\"%s\",\"d\":[", MUSASHI_GIT_REVISION);
    for (index = 0; index < 8; ++index) {
        printf("%s\"%08X\"", index ? "," : "", m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + index)));
    }
    printf("],\"pc\":\"%08X\",\"sr\":\"%04X\",\"writes\":%lu,\"accesses\":[",
           m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFU, writes);
    for (index = 0; (unsigned long)index < operand_read_count; ++index) {
        printf("%s{\"kind\":\"operand_read\",\"address\":\"%08X\",\"width\":32,\"value\":\"%08X\"}",
               index ? "," : "", operand_reads[index].address, operand_reads[index].value);
    }
    printf("]}\n");
    free(image);
    return 0;
}
