#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"
static unsigned char mem[0x1000000];
unsigned int m68k_read_disassembler_8(unsigned int a){return mem[a&0xffffff];}
unsigned int m68k_read_disassembler_16(unsigned int a){a&=0xffffff;return mem[a]<<8|mem[a+1];}
unsigned int m68k_read_disassembler_32(unsigned int a){return m68k_read_disassembler_16(a)<<16|m68k_read_disassembler_16(a+2);}
unsigned int m68k_read_memory_8(unsigned int a){return 0;} unsigned int m68k_read_memory_16(unsigned int a){return 0;} unsigned int m68k_read_memory_32(unsigned int a){return 0;}
void m68k_write_memory_8(unsigned int a,unsigned int v){} void m68k_write_memory_16(unsigned int a,unsigned int v){} void m68k_write_memory_32(unsigned int a,unsigned int v){}
/* usage: dasm file loadaddr fileoffset start end */
int main(int c,char**v){FILE*f=fopen(v[1],"rb");unsigned load=strtoul(v[2],0,0),off=strtoul(v[3],0,0),s=strtoul(v[4],0,0),e=strtoul(v[5],0,0);
fseek(f,off,0);fread(mem+load,1,0x1000000-load,f);char b[256];
for(unsigned pc=s;pc<e;){unsigned n=m68k_disassemble(b,pc,M68K_CPU_TYPE_68000);printf("%06X: ",pc);for(unsigned i=0;i<n;i+=2)printf("%04X ",m68k_read_disassembler_16(pc+i));for(unsigned i=n;i<10;i+=2)printf("     ");printf(" %s\n",b);pc+=n;}}
