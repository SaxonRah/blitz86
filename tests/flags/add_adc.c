#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "b86.h"
static uint8_t memA[B86_MEM_BYTES] __attribute__((aligned(64))), memB[B86_MEM_BYTES] __attribute__((aligned(64)));
/* code at 1000:0000 */
static const uint8_t code[] = {
  0xB9,0x07,0x00,             /* mov cx,7 */
  /* L (3): */
  0x00,0x04,                  /* add [si],al          */
  0x13,0x1D,                  /* adc bx,[di]          */
  0x9C,0x5D,                  /* pushf; pop bp        */
  0x89,0x2E,0x00,0x20,        /* mov [2000h],bp       */
  0x28,0x64,0x02,             /* sub [si+2],ah        */
  0x1A,0x55,0x03,             /* sbb dl,[di+3]        */
  0x83,0xC2,0x7F,             /* add dx,7fh           */
  0x11,0xD0,                  /* adc ax,dx            */
  0x3B,0x05,                  /* cmp ax,[di]          */
  0x83,0xD6,0x00,             /* adc si,0             */
  0x81,0xE6,0xFF,0x0F,        /* and si,0fffh         */
  0x81,0xE7,0xFF,0x0F,        /* and di,0fffh         */
  0x47,                       /* inc di               */
  0xE2,0xDB,                  /* loop L */
  0x9C, 0x5F,                 /* pushf; pop di */
  0xF4 };
static void init(B86Cpu *c, uint8_t *m, unsigned seed){
  memset(m,0,B86_MEM_BYTES); srand(seed);
  memcpy(m+0x10000, code, sizeof code);
  for (int i=0;i<0x10000;i++) m[0x20000+i]=rand();
  b86_init(c,m); b86_set_seg(c,B86_CS,0x1000); b86_set_seg(c,B86_DS,0x2000); b86_set_seg(c,B86_SS,0x3000);b86_set_seg(c,B86_ES,0x2000);
  c->r[B86_SP]=0xFFF0; c->ip=0;
  for (int r=0;r<8;r++) if (r!=B86_SP) c->r[r]=rand()&0xFFFF;
  c->r[B86_CX]=0; b86_set_flags(c, rand() & 0x8D5);
}
int main(int argc,char**argv){
  size_t sz=1<<20; void*buf=mmap(NULL,sz,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  int bad=0;
  for (unsigned s=1;s<=3000;s++){
    B86Cpu a,b; init(&a,memA,s); init(&b,memB,s);
    b86_run_interp(&a, 100000);
    struct B86Jit*j=b86_jit_create(&b,buf,sz); b86_jit_run(&b,~0ull);
    int rd = 0; for (int r = 0; r < 8; r++) rd |= (uint16_t)a.r[r] != (uint16_t)b.r[r];
    int diff = rd || b86_get_flags(&a)!=b86_get_flags(&b) || memcmp(memA+0x20000,memB+0x20000,0x10000);
    if (diff && bad < 1) { for (int r=0;r<8;r++) if ((uint16_t)a.r[r]!=(uint16_t)b.r[r]) printf(" r%d %x/%x", r, (unsigned)a.r[r], (unsigned)b.r[r]); for (int k=0;k<0x10000;k++) if (memA[0x20000+k]!=memB[0x20000+k]) { printf(" mem %x %02x/%02x", k, memA[0x20000+k], memB[0x20000+k]); break; } printf("\n"); }
    if (diff){ if(++bad<5) printf("seed %u mismatch flags %04x/%04x bp %04x/%04x di %04x/%04x\n",s,b86_get_flags(&a),b86_get_flags(&b),(unsigned)a.r[5],(unsigned)b.r[5],(unsigned)a.r[7],(unsigned)b.r[7]); }
    { static unsigned long long rf; rf += b86_jit_stats(j)->rt_flags; if (s == 3000) printf("rt_flags total %llu\n", rf); }
    b86_jit_destroy(j);
  }
  printf("rcx test: %d mismatches\n",bad); return bad!=0;
}
