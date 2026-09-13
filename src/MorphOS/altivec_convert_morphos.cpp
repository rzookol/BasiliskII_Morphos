#include <altivec.h>
#include "altivec_morphos.h"

#if defined(__GNUC__)
#define BII_NOINLINE __attribute__((noinline))
#else
#define BII_NOINLINE
#endif

static const vector unsigned short k_mask31 = {31,31,31,31,31,31,31,31};
static const vector unsigned char k_swap16 = {1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14};

static int aligned16(const void *a, const void *b)
{
    return ((((unsigned long)a | (unsigned long)b) & 15UL) == 0);
}

int BasiliskMorphOSAltiVecCompiled(void) { return 1; }

BII_NOINLINE uint32 BasiliskMorphOSAltiVecRGB565ToRGB16PC(const uint8 *src, uint8 *dst, uint32 pixels)
{
    uint32 x, count = pixels & ~15U;
    if (!aligned16(src, dst)) return 0;
    for (x = 0; x < count; x += 16) {
        const uint8 *s = src + x * 2;
        uint8 *d = dst + x * 2;
        vector unsigned char p0 = vec_ld(0, s);
        vector unsigned char p1 = vec_ld(16, s);
        vec_st(vec_perm(p0, p0, k_swap16), 0, d);
        vec_st(vec_perm(p1, p1, k_swap16), 16, d);
    }
    return count;
}

static inline vector unsigned short rgb555_to_565(vector unsigned short p)
{
    const vector unsigned short sh1 = vec_splat_u16(1);
    const vector unsigned short sh4 = vec_splat_u16(4);
    const vector unsigned short sh5 = vec_splat_u16(5);
    const vector unsigned short sh10 = vec_splat_u16(10);
    const vector unsigned short sh11 = vec_splat_u16(11);
    vector unsigned short r = vec_and(vec_sr(p, sh10), k_mask31);
    vector unsigned short g = vec_and(vec_sr(p, sh5), k_mask31);
    vector unsigned short b = vec_and(p, k_mask31);
    vector unsigned short g6 = vec_or(vec_sl(g, sh1), vec_sr(g, sh4));
    return vec_or(vec_or(vec_sl(r, sh11), vec_sl(g6, sh5)), b);
}

BII_NOINLINE uint32 BasiliskMorphOSAltiVecRGB555ToRGB16PC(const uint8 *src, uint8 *dst, uint32 pixels)
{
    uint32 x, count = pixels & ~15U;
    if (!aligned16(src, dst)) return 0;
    for (x = 0; x < count; x += 16) {
        const uint8 *s = src + x * 2;
        uint8 *d = dst + x * 2;
        vector unsigned short p0 = (vector unsigned short)vec_ld(0, s);
        vector unsigned short p1 = (vector unsigned short)vec_ld(16, s);
        vector unsigned char q0 = (vector unsigned char)rgb555_to_565(p0);
        vector unsigned char q1 = (vector unsigned char)rgb555_to_565(p1);
        vec_st(vec_perm(q0, q0, k_swap16), 0, d);
        vec_st(vec_perm(q1, q1, k_swap16), 16, d);
    }
    return count;
}

static uint16 rgb16pc(uint8 r, uint8 g, uint8 b)
{
    uint16 r5 = r >> 3, g6 = g >> 2, b5 = b >> 3;
    return (uint16)(((g6 & 7) << 13) | (b5 << 8) | (r5 << 3) | ((g6 >> 3) & 7));
}

int BasiliskMorphOSAltiVecSelfTest(void)
{
    uint8 src[32] __attribute__((aligned(16)));
    uint8 dst[32] __attribute__((aligned(16)));
    uint8 expected[32] __attribute__((aligned(16)));
    uint32 i;
    for (i = 0; i < 16; ++i) {
        uint16 p = (uint16)((i * 1987U + 0x1234U) & 0x7fffU);
        uint8 r5=(p>>10)&31, g5=(p>>5)&31, b5=p&31;
        uint16 q=rgb16pc((r5<<3)|(r5>>2),(g5<<3)|(g5>>2),(b5<<3)|(b5>>2));
        src[i*2]=p>>8; src[i*2+1]=p;
        expected[i*2]=q>>8; expected[i*2+1]=q;
        dst[i*2]=dst[i*2+1]=0;
    }
    if (BasiliskMorphOSAltiVecRGB555ToRGB16PC(src,dst,16) != 16) return 0;
    for (i=0;i<32;i++) if (dst[i]!=expected[i]) return 0;
    return 1;
}
