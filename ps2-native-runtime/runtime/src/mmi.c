// The R5900's 128-bit multimedia instructions. Each reads its operands into locals first: rd may be rs or rt. A write to r0 is
// dropped (the instructions that also write HI and LO still do that).
#include "ps2/recomp.h"

#define RS (c->r[rs])
#define RT (c->r[rt])
#define OUT(v)                                                                                                                 \
    do                                                                                                                         \
    {                                                                                                                          \
        if (rd != 0)                                                                                                           \
            c->r[rd] = (v);                                                                                                    \
    } while (0)

static inline s32 sat32(s64 v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF - 1 ? (s32)0x80000000 : (s32)v; }
static inline s16 sat16(s32 v) { return v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : (s16)v; }
static inline s8 sat8(s32 v) { return v > 0x7F ? 0x7F : v < -0x80 ? -0x80 : (s8)v; }
static inline u32 usat32(s64 v) { return v > 0xFFFFFFFFll ? 0xFFFFFFFFu : v < 0 ? 0 : (u32)v; }
static inline u16 usat16(s32 v) { return v > 0xFFFF ? 0xFFFF : v < 0 ? 0 : (u16)v; }
static inline u8 usat8(s32 v) { return v > 0xFF ? 0xFF : v < 0 ? 0 : (u8)v; }

#define LANES(N, field, expr)                                                                                                  \
    void mmi_##N(Ps2Cpu* c, int rd, int rs, int rt)                                                                            \
    {                                                                                                                          \
        R128 a = RS, b = RT, r;                                                                                                \
        for (int k = 0; k < (int)(sizeof(r.field) / sizeof(r.field[0])); k++)                                                  \
            r.field[k] = (expr);                                                                                               \
        OUT(r);                                                                                                                \
    }

LANES(paddw, uw, a.uw[k] + b.uw[k])
LANES(psubw, uw, a.uw[k] - b.uw[k])
LANES(pcgtw, uw, a.sw[k] > b.sw[k] ? 0xFFFFFFFFu : 0)
LANES(pmaxw, sw, a.sw[k] > b.sw[k] ? a.sw[k] : b.sw[k])
LANES(paddh, uh, (u16)(a.uh[k] + b.uh[k]))
LANES(psubh, uh, (u16)(a.uh[k] - b.uh[k]))
LANES(pcgth, uh, a.sh[k] > b.sh[k] ? 0xFFFF : 0)
LANES(pmaxh, sh, a.sh[k] > b.sh[k] ? a.sh[k] : b.sh[k])
LANES(paddb, ub, (u8)(a.ub[k] + b.ub[k]))
LANES(psubb, ub, (u8)(a.ub[k] - b.ub[k]))
LANES(pcgtb, ub, a.sb[k] > b.sb[k] ? 0xFF : 0)
LANES(paddsw, sw, sat32((s64)a.sw[k] + b.sw[k]))
LANES(psubsw, sw, sat32((s64)a.sw[k] - b.sw[k]))
LANES(paddsh, sh, sat16((s32)a.sh[k] + b.sh[k]))
LANES(psubsh, sh, sat16((s32)a.sh[k] - b.sh[k]))
LANES(paddsb, sb, sat8((s32)a.sb[k] + b.sb[k]))
LANES(psubsb, sb, sat8((s32)a.sb[k] - b.sb[k]))
LANES(pabsw, sw, b.sw[k] == (s32)0x80000000 ? 0x7FFFFFFF : (b.sw[k] < 0 ? -b.sw[k] : b.sw[k]))
LANES(pabsh, sh, b.sh[k] == -0x8000 ? 0x7FFF : (b.sh[k] < 0 ? -b.sh[k] : b.sh[k]))
LANES(pceqw, uw, a.uw[k] == b.uw[k] ? 0xFFFFFFFFu : 0)
LANES(pceqh, uh, a.uh[k] == b.uh[k] ? 0xFFFF : 0)
LANES(pceqb, ub, a.ub[k] == b.ub[k] ? 0xFF : 0)
LANES(pminw, sw, a.sw[k] < b.sw[k] ? a.sw[k] : b.sw[k])
LANES(pminh, sh, a.sh[k] < b.sh[k] ? a.sh[k] : b.sh[k])
LANES(padduw, uw, usat32((s64)a.uw[k] + b.uw[k]))
LANES(psubuw, uw, usat32((s64)a.uw[k] - b.uw[k]))
LANES(padduh, uh, usat16((s32)a.uh[k] + b.uh[k]))
LANES(psubuh, uh, usat16((s32)a.uh[k] - b.uh[k]))
LANES(paddub, ub, usat8((s32)a.ub[k] + b.ub[k]))
LANES(psubub, ub, usat8((s32)a.ub[k] - b.ub[k]))
LANES(padsbh, uh, k < 4 ? (u16)(a.uh[k] - b.uh[k]) : (u16)(a.uh[k] + b.uh[k]))

void mmi_pand(Ps2Cpu* c, int rd, int rs, int rt) { R128 r; r.q = RS.q & RT.q; OUT(r); }
void mmi_por(Ps2Cpu* c, int rd, int rs, int rt) { R128 r; r.q = RS.q | RT.q; OUT(r); }
void mmi_pxor(Ps2Cpu* c, int rd, int rs, int rt) { R128 r; r.q = RS.q ^ RT.q; OUT(r); }
void mmi_pnor(Ps2Cpu* c, int rd, int rs, int rt) { R128 r; r.q = ~(RS.q | RT.q); OUT(r); }

void mmi_pextlw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.uw[0] = b.uw[0]; r.uw[1] = a.uw[0]; r.uw[2] = b.uw[1]; r.uw[3] = a.uw[1];
    OUT(r);
}
void mmi_pextuw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.uw[0] = b.uw[2]; r.uw[1] = a.uw[2]; r.uw[2] = b.uw[3]; r.uw[3] = a.uw[3];
    OUT(r);
}
void mmi_ppacw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.uw[0] = b.uw[0]; r.uw[1] = b.uw[2]; r.uw[2] = a.uw[0]; r.uw[3] = a.uw[2];
    OUT(r);
}
void mmi_pextlh(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 4; k++) { r.uh[2 * k] = b.uh[k]; r.uh[2 * k + 1] = a.uh[k]; }
    OUT(r);
}
void mmi_pextuh(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 4; k++) { r.uh[2 * k] = b.uh[4 + k]; r.uh[2 * k + 1] = a.uh[4 + k]; }
    OUT(r);
}
void mmi_ppach(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 4; k++) { r.uh[k] = b.uh[2 * k]; r.uh[4 + k] = a.uh[2 * k]; }
    OUT(r);
}
void mmi_pextlb(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 8; k++) { r.ub[2 * k] = b.ub[k]; r.ub[2 * k + 1] = a.ub[k]; }
    OUT(r);
}
void mmi_pextub(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 8; k++) { r.ub[2 * k] = b.ub[8 + k]; r.ub[2 * k + 1] = a.ub[8 + k]; }
    OUT(r);
}
void mmi_ppacb(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 8; k++) { r.ub[k] = b.ub[2 * k]; r.ub[8 + k] = a.ub[2 * k]; }
    OUT(r);
}
void mmi_pext5(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 b = RT, r;
    (void)rs;
    for (int k = 0; k < 4; k++)
    {
        u32 w = b.uw[k];
        r.uw[k] = ((w & 0x1F) << 3) | (((w >> 5) & 0x1F) << 11) | (((w >> 10) & 0x1F) << 19) | (((w >> 15) & 1) << 31);
    }
    OUT(r);
}
void mmi_ppac5(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 b = RT, r;
    (void)rs;
    for (int k = 0; k < 4; k++)
    {
        u32 w = b.uw[k];
        r.uw[k] = ((w >> 3) & 0x1F) | (((w >> 11) & 0x1F) << 5) | (((w >> 19) & 0x1F) << 10) | (((w >> 31) & 1) << 15);
    }
    OUT(r);
}
void mmi_qfsrv(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    u32 shift = (c->sa & 15) * 8;
    if (shift == 0)
        r = b;
    else
        r.q = (b.q >> shift) | (a.q << (128 - shift));
    OUT(r);
}

// HI and LO: pipeline 0's word pairs are words 0-1, pipeline 1's words 2-3
void mmi_pmaddw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 2; k++)
    {
        s64 acc = (s64)(((u64)c->hi.uw[2 * k] << 32) | c->lo.uw[2 * k]);
        s64 t = acc + (s64)a.sw[2 * k] * b.sw[2 * k];
        c->lo.sd[k] = (s32)t;
        c->hi.sd[k] = (s32)(t >> 32);
        r.sd[k] = t;
    }
    OUT(r);
}
void mmi_pmsubw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 2; k++)
    {
        s64 acc = (s64)(((u64)c->hi.uw[2 * k] << 32) | c->lo.uw[2 * k]);
        s64 t = acc - (s64)a.sw[2 * k] * b.sw[2 * k];
        c->lo.sd[k] = (s32)t;
        c->hi.sd[k] = (s32)(t >> 32);
        r.sd[k] = t;
    }
    OUT(r);
}
void mmi_pmadduw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 2; k++)
    {
        u64 acc = ((u64)c->hi.uw[2 * k] << 32) | c->lo.uw[2 * k];
        u64 t = acc + (u64)a.uw[2 * k] * b.uw[2 * k];
        c->lo.sd[k] = (s32)(u32)t;
        c->hi.sd[k] = (s32)(u32)(t >> 32);
        r.ud[k] = t;
    }
    OUT(r);
}
void mmi_pmultw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 2; k++)
    {
        s64 t = (s64)a.sw[2 * k] * b.sw[2 * k];
        c->lo.sd[k] = (s32)t;
        c->hi.sd[k] = (s32)(t >> 32);
        r.sd[k] = t;
    }
    OUT(r);
}
void mmi_pmultuw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 2; k++)
    {
        u64 t = (u64)a.uw[2 * k] * b.uw[2 * k];
        c->lo.sd[k] = (s32)(u32)t;
        c->hi.sd[k] = (s32)(u32)(t >> 32);
        r.ud[k] = t;
    }
    OUT(r);
}
void mmi_pdivw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT;
    (void)rd;
    for (int k = 0; k < 2; k++)
        ps2_div(c, a.sw[2 * k], b.sw[2 * k], k);
}
void mmi_pdivuw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT;
    (void)rd;
    for (int k = 0; k < 2; k++)
        ps2_divu(c, a.uw[2 * k], b.uw[2 * k], k);
}
void mmi_pdivbw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT;
    s32 d = b.sh[0];
    (void)rd;
    for (int k = 0; k < 4; k++)
    {
        s32 q, rem;
        if (d == 0) { q = a.sw[k] >= 0 ? -1 : 1; rem = a.sw[k]; }
        else if (a.sw[k] == (s32)0x80000000 && d == -1) { q = (s32)0x80000000; rem = 0; }
        else { q = a.sw[k] / d; rem = a.sw[k] % d; }
        c->lo.sw[k] = q;
        c->hi.sw[k] = rem;
    }
}
void mmi_psllvw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.sd[0] = (s32)(b.uw[0] << (a.uw[0] & 31));
    r.sd[1] = (s32)(b.uw[2] << (a.uw[2] & 31));
    OUT(r);
}
void mmi_psrlvw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.sd[0] = (s32)(b.uw[0] >> (a.uw[0] & 31));
    r.sd[1] = (s32)(b.uw[2] >> (a.uw[2] & 31));
    OUT(r);
}
void mmi_psravw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.sd[0] = b.sw[0] >> (a.uw[0] & 31);
    r.sd[1] = b.sw[2] >> (a.uw[2] & 31);
    OUT(r);
}
void mmi_pmfhi(Ps2Cpu* c, int rd, int rs, int rt) { (void)rs; (void)rt; OUT(c->hi); }
void mmi_pmflo(Ps2Cpu* c, int rd, int rs, int rt) { (void)rs; (void)rt; OUT(c->lo); }
void mmi_pmthi(Ps2Cpu* c, int rd, int rs, int rt) { (void)rd; (void)rt; c->hi = RS; }
void mmi_pmtlo(Ps2Cpu* c, int rd, int rs, int rt) { (void)rd; (void)rt; c->lo = RS; }
void mmi_pinth(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 4; k++) { r.uh[2 * k] = b.uh[k]; r.uh[2 * k + 1] = a.uh[4 + k]; }
    OUT(r);
}
void mmi_pinteh(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 4; k++) { r.uh[2 * k] = b.uh[2 * k]; r.uh[2 * k + 1] = a.uh[2 * k]; }
    OUT(r);
}
void mmi_pcpyld(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.ud[0] = b.ud[0]; r.ud[1] = a.ud[0];
    OUT(r);
}
void mmi_pcpyud(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, b = RT, r;
    r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1];
    OUT(r);
}
void mmi_pcpyh(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 b = RT, r;
    (void)rs;
    for (int k = 0; k < 4; k++) { r.uh[k] = b.uh[0]; r.uh[4 + k] = b.uh[4]; }
    OUT(r);
}
static void halves(Ps2Cpu* c, int rd, int rt, const int* order)
{
    R128 b = RT, r;
    for (int k = 0; k < 8; k++)
        r.uh[k] = b.uh[order[k]];
    OUT(r);
}
static void words(Ps2Cpu* c, int rd, int rt, const int* order)
{
    R128 b = RT, r;
    for (int k = 0; k < 4; k++)
        r.uw[k] = b.uw[order[k]];
    OUT(r);
}
void mmi_pexeh(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[8] = {2, 1, 0, 3, 6, 5, 4, 7}; (void)rs; halves(c, rd, rt, o); }
void mmi_prevh(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[8] = {3, 2, 1, 0, 7, 6, 5, 4}; (void)rs; halves(c, rd, rt, o); }
void mmi_pexch(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[8] = {0, 2, 1, 3, 4, 6, 5, 7}; (void)rs; halves(c, rd, rt, o); }
void mmi_pexew(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[4] = {2, 1, 0, 3}; (void)rs; words(c, rd, rt, o); }
void mmi_prot3w(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[4] = {1, 2, 0, 3}; (void)rs; words(c, rd, rt, o); }
void mmi_pexcw(Ps2Cpu* c, int rd, int rs, int rt) { static const int o[4] = {0, 2, 1, 3}; (void)rs; words(c, rd, rt, o); }

// The halfword multiplies: products k go to LO words 0,1 / HI 0,1 / LO 2,3 / HI 2,3 in pairs
static const int HW_SLOT[8] = {0, 1, 0, 1, 2, 3, 2, 3};
static const int HW_HI[8] = {0, 0, 1, 1, 0, 0, 1, 1};
static void halfword_madd(Ps2Cpu* c, int rd, int rs, int rt, int mode)
{
    R128 a = RS, b = RT, r;
    for (int k = 0; k < 8; k++)
    {
        s32 p = (s32)a.sh[k] * (s32)b.sh[k];
        s32* dst = HW_HI[k] ? &c->hi.sw[HW_SLOT[k]] : &c->lo.sw[HW_SLOT[k]];
        if (mode == 0)
            *dst = p;
        else if (mode == 1)
            *dst += p;
        else
            *dst -= p;
    }
    r.uw[0] = c->lo.uw[0];
    r.uw[1] = c->hi.uw[0];
    r.uw[2] = c->lo.uw[2];
    r.uw[3] = c->hi.uw[2];
    OUT(r);
}
void mmi_pmulth(Ps2Cpu* c, int rd, int rs, int rt) { halfword_madd(c, rd, rs, rt, 0); }
void mmi_pmaddh(Ps2Cpu* c, int rd, int rs, int rt) { halfword_madd(c, rd, rs, rt, 1); }
void mmi_pmsubh(Ps2Cpu* c, int rd, int rs, int rt) { halfword_madd(c, rd, rs, rt, 2); }
static void halfword_hmadd(Ps2Cpu* c, int rd, int rs, int rt, int sub)
{
    R128 a = RS, b = RT, r;
    s32 v[4];
    for (int k = 0; k < 4; k++)
    {
        s32 hi = (s32)a.sh[2 * k + 1] * b.sh[2 * k + 1];
        s32 lo = (s32)a.sh[2 * k] * b.sh[2 * k];
        v[k] = sub ? hi - lo : hi + lo;
    }
    c->lo.sw[0] = v[0];
    c->hi.sw[0] = v[1];
    c->lo.sw[2] = v[2];
    c->hi.sw[2] = v[3];
    r.sw[0] = v[0];
    r.sw[1] = v[1];
    r.sw[2] = v[2];
    r.sw[3] = v[3];
    OUT(r);
}
void mmi_phmadh(Ps2Cpu* c, int rd, int rs, int rt) { halfword_hmadd(c, rd, rs, rt, 0); }
void mmi_phmsbh(Ps2Cpu* c, int rd, int rs, int rt) { halfword_hmadd(c, rd, rs, rt, 1); }

void mmi_plzcw(Ps2Cpu* c, int rd, int rs, int rt)
{
    R128 a = RS, r = c->r[rd];
    (void)rt;
    for (int k = 0; k < 2; k++)
    {
        u32 v = a.uw[k];
        if (v & 0x80000000u)
            v = ~v;
        r.uw[k] = v == 0 ? 31 : (u32)__builtin_clz(v) - 1;
    }
    OUT(r);
}

#define SHIFT(N, field, lanes, expr)                                                                                           \
    void mmi_##N(Ps2Cpu* c, int rd, int rt, int rs, int sa)                                                                    \
    {                                                                                                                          \
        R128 b = RT, r;                                                                                                        \
        (void)rs;                                                                                                              \
        for (int k = 0; k < lanes; k++)                                                                                        \
            r.field[k] = (expr);                                                                                               \
        OUT(r);                                                                                                                \
    }
SHIFT(psllh, uh, 8, (u16)(b.uh[k] << (sa & 15)))
SHIFT(psrlh, uh, 8, (u16)(b.uh[k] >> (sa & 15)))
SHIFT(psrah, sh, 8, (s16)(b.sh[k] >> (sa & 15)))
SHIFT(psllw, uw, 4, b.uw[k] << (sa & 31))
SHIFT(psrlw, uw, 4, b.uw[k] >> (sa & 31))
SHIFT(psraw, sw, 4, b.sw[k] >> (sa & 31))

void mmi_pmfhl(Ps2Cpu* c, int rd, int rt, int rs, int sa)
{
    R128 lo = c->lo, hi = c->hi, r;
    (void)rt;
    (void)rs;
    switch (sa)
    {
    case 0: // LW
        r.uw[0] = lo.uw[0]; r.uw[1] = hi.uw[0]; r.uw[2] = lo.uw[2]; r.uw[3] = hi.uw[2];
        break;
    case 1: // UW
        r.uw[0] = lo.uw[1]; r.uw[1] = hi.uw[1]; r.uw[2] = lo.uw[3]; r.uw[3] = hi.uw[3];
        break;
    case 2: // SLW
        for (int k = 0; k < 2; k++)
        {
            s64 v = (s64)(((u64)hi.uw[2 * k] << 32) | lo.uw[2 * k]);
            r.sd[k] = sat32(v);
        }
        break;
    case 3: // LH
        r.uh[0] = lo.uh[0]; r.uh[1] = lo.uh[2]; r.uh[2] = hi.uh[0]; r.uh[3] = hi.uh[2];
        r.uh[4] = lo.uh[4]; r.uh[5] = lo.uh[6]; r.uh[6] = hi.uh[4]; r.uh[7] = hi.uh[6];
        break;
    default: // SH
        r.sh[0] = sat16(lo.sw[0]); r.sh[1] = sat16(lo.sw[1]); r.sh[2] = sat16(hi.sw[0]); r.sh[3] = sat16(hi.sw[1]);
        r.sh[4] = sat16(lo.sw[2]); r.sh[5] = sat16(lo.sw[3]); r.sh[6] = sat16(hi.sw[2]); r.sh[7] = sat16(hi.sw[3]);
        break;
    }
    OUT(r);
}
void mmi_pmthl(Ps2Cpu* c, int rd, int rt, int rs, int sa)
{
    R128 a = RS;
    (void)rd;
    (void)rt;
    if (sa == 0)
    {
        c->lo.uw[0] = a.uw[0];
        c->hi.uw[0] = a.uw[1];
        c->lo.uw[2] = a.uw[2];
        c->hi.uw[2] = a.uw[3];
    }
}
