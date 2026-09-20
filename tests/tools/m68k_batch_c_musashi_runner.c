/* Test-only pinned Musashi runner for the project-authored Batch-C vectors. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#define RAM_BEGIN UINT32_C(0x00FF0000)
#define RAM_SIZE UINT32_C(0x00010000)
#ifndef MUSASHI_GIT_REVISION
#error "MUSASHI_GIT_REVISION must be supplied by the validator"
#endif
static uint8_t *image; static size_t image_size; static uint32_t image_begin; static uint8_t ram[RAM_SIZE];
static unsigned long writes, ram_reads;
static unsigned int read8(unsigned int address) { if (address >= image_begin && (uint64_t)(address - image_begin) < image_size) return image[address - image_begin]; if (address >= RAM_BEGIN && address < RAM_BEGIN + RAM_SIZE) { ++ram_reads; return ram[address - RAM_BEGIN]; } return 0U; }
static void write8(unsigned int address, unsigned int value) { ++writes; if (address >= RAM_BEGIN && address < RAM_BEGIN + RAM_SIZE) ram[address - RAM_BEGIN] = (uint8_t)value; }
static unsigned int read16(unsigned int address) { return (read8(address) << 8U) | read8(address + 1U); }
static unsigned int read32(unsigned int address) { return (read16(address) << 16U) | read16(address + 2U); }
unsigned int m68k_read_memory_8(unsigned int a) { return read8(a); } unsigned int m68k_read_memory_16(unsigned int a) { return read16(a); } unsigned int m68k_read_memory_32(unsigned int a) { return read32(a); }
unsigned int m68k_read_immediate_16(unsigned int a) { return read16(a); } unsigned int m68k_read_immediate_32(unsigned int a) { return read32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return read8(a); } unsigned int m68k_read_pcrelative_16(unsigned int a) { return read16(a); } unsigned int m68k_read_pcrelative_32(unsigned int a) { return read32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return read8(a); } unsigned int m68k_read_disassembler_16(unsigned int a) { return read16(a); } unsigned int m68k_read_disassembler_32(unsigned int a) { return read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { write8(a,v); } void m68k_write_memory_16(unsigned int a,unsigned int v) { write8(a,v>>8U);write8(a+1U,v); } void m68k_write_memory_32(unsigned int a,unsigned int v) { m68k_write_memory_16(a,v>>16U);m68k_write_memory_16(a+2U,v); } void m68k_write_memory_32_pd(unsigned int a,unsigned int v) { m68k_write_memory_32(a,v); }
static uint32_t parse(const char *s, size_t n) { char *end; unsigned long v; if(strlen(s)!=n || (v=strtoul(s,&end,16))>UINT32_MAX || *end) exit(2); return (uint32_t)v; }
int main(int argc,char **argv) { FILE *f; long n; uint32_t d[8],a[8]; int i;
  if(argc<20 || (argc-20)%2) return 2; f=fopen(argv[1],"rb"); if(!f || fseek(f,0,SEEK_END)||(n=ftell(f))<0||n>0x400000L) return 2; rewind(f); image_size=(size_t)n; image=calloc(image_size+1U,1); if(!image || (image_size&&fread(image,1,image_size,f)!=image_size)) return 2; fclose(f);
  for(i=0;i<8;++i)d[i]=parse(argv[4+i],8); for(i=0;i<8;++i)a[i]=parse(argv[12+i],8); for(i=20;i<argc;i+=2) { uint32_t p=parse(argv[i],8),v=parse(argv[i+1],8); if(p<RAM_BEGIN||(uint64_t)p+4U>RAM_BEGIN+RAM_SIZE)return 2; ram[p-RAM_BEGIN]=(uint8_t)(v>>24);ram[p-RAM_BEGIN+1]=(uint8_t)(v>>16);ram[p-RAM_BEGIN+2]=(uint8_t)(v>>8);ram[p-RAM_BEGIN+3]=(uint8_t)v; }
  image_begin=parse(argv[2],8);m68k_init();m68k_set_cpu_type(M68K_CPU_TYPE_68000);m68k_pulse_reset();(void)m68k_execute(132);for(i=0;i<8;++i)m68k_set_reg((m68k_register_t)(M68K_REG_D0+i),d[i]);for(i=0;i<8;++i)m68k_set_reg((m68k_register_t)(M68K_REG_A0+i),a[i]);m68k_set_reg(M68K_REG_PC,image_begin);m68k_set_reg(M68K_REG_SR,parse(argv[3],4));writes=ram_reads=0;(void)m68k_execute(0);
  printf("{\"musashi_revision\":\"%s\",\"d\":[",MUSASHI_GIT_REVISION);for(i=0;i<8;++i)printf("%s\"%08X\"",i?",":"",m68k_get_reg(NULL,(m68k_register_t)(M68K_REG_D0+i)));printf("],\"a\":[");for(i=0;i<8;++i)printf("%s\"%08X\"",i?",":"",m68k_get_reg(NULL,(m68k_register_t)(M68K_REG_A0+i)));printf("],\"pc\":\"%08X\",\"sr\":\"%04X\",\"ram_writes\":%lu,\"ram_reads\":%lu,\"ram\":[",m68k_get_reg(NULL,M68K_REG_PC),m68k_get_reg(NULL,M68K_REG_SR)&0xffffU,writes,ram_reads);for(i=20;i<argc;i+=2)printf("%s\"%08X\"",i==20?"":",",read32(parse(argv[i],8)));printf("]}\n");free(image);return 0; }
