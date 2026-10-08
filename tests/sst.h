/* SingleStepTests binary vector reader (see tools/sst_convert.py). */
#ifndef SST_H
#define SST_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint32_t a; uint8_t v; } SstByte;
typedef struct {
    uint8_t op, reg;
    uint16_t init[14], fin[14];
    uint16_t fmask;
    uint16_t ni, nf, ng;
    SstByte ri[2048], rf[2048];
    uint32_t ign[4];
} SstVec;

/* register order in the file */
enum { S_AX, S_BX, S_CX, S_DX, S_CS, S_SS, S_DS, S_ES, S_SP, S_BP, S_SI, S_DI, S_IP, S_FL };

static int sst_read(FILE *f, SstVec *t)
{
    uint8_t h[2];
    if (fread(h, 1, 2, f) != 2) return 0;
    t->op = h[0]; t->reg = h[1];
    if (fread(t->init, 2, 14, f) != 14 || fread(t->fin, 2, 14, f) != 14) return 0;
    uint16_t x[4];
    if (fread(x, 2, 4, f) != 4) return 0;
    t->fmask = x[0]; t->ni = x[1]; t->nf = x[2]; t->ng = x[3];
    if (t->ni > 2048 || t->nf > 2048 || t->ng > 4) { fprintf(stderr, "vector too large\n"); exit(2); }
    for (int i = 0; i < t->ni; ++i) { uint8_t b[5]; if (fread(b, 1, 5, f) != 5) return 0;
        t->ri[i].a = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; t->ri[i].v = b[4]; }
    for (int i = 0; i < t->nf; ++i) { uint8_t b[5]; if (fread(b, 1, 5, f) != 5) return 0;
        t->rf[i].a = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; t->rf[i].v = b[4]; }
    for (int i = 0; i < t->ng; ++i) if (fread(&t->ign[i], 4, 1, f) != 1) return 0;
    return 1;
}
#endif
