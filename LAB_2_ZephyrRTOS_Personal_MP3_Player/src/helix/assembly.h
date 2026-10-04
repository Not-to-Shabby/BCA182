#ifndef _ASSEMBLY_H
#define _ASSEMBLY_H

/* Cortex-M4 / GCC replacements for the multiply and shift primitives Helix expects. */

typedef long long Word64;

static inline int MULSHIFT32(int x, int y)
{
    int result;
    __asm__("smmul %0, %1, %2" : "=r"(result) : "r"(x), "r"(y));
    return result;
}

static inline int FASTABS(int x)
{
    int sign = x >> 31;
    return (x ^ sign) - sign;
}

static inline int CLZ(int x)
{
    return x ? __builtin_clz((unsigned int)x) : 32;
}

/* GCC emits SMLAL for this at -O1 and above. */
static inline Word64 MADD64(Word64 sum, int x, int y)
{
    return sum + (Word64)x * y;
}

static inline Word64 SHL64(Word64 x, int n)
{
    return x << n;
}

static inline Word64 SAR64(Word64 x, int n)
{
    return x >> n;
}

#endif /* _ASSEMBLY_H */
