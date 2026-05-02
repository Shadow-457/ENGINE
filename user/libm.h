/* ================================================================
 *  Systrix OS — user/libm.h
 *  Software math library — drop-in <math.h> replacement.
 *
 *  All functions implemented in pure C using double arithmetic.
 *  Compiler uses x87/SSE in userspace (no -mno-sse in UCFLAGS).
 *
 *  Covers everything CSS transforms, canvas 2D, and JS Math need:
 *    Trig:    sin cos tan asin acos atan atan2
 *    Exp/log: exp exp2 log log2 log10 pow
 *    Round:   floor ceil round trunc fabs fmod
 *    Misc:    sqrt hypot ldexp frexp modf copysign
 *    Class:   isinf isnan isfinite
 * ================================================================ */

#pragma once

/* ── Bit-level double helpers ────────────────────────────────── */
static inline unsigned long long _d2u(double d) {
    union { double d; unsigned long long u; } v; v.d = d; return v.u;
}
static inline double _u2d(unsigned long long u) {
    union { double d; unsigned long long u; } v; v.u = u; return v.d;
}

/* ── Constants ───────────────────────────────────────────────── */
#define M_E         2.718281828459045235360
#define M_LOG2E     1.442695040888963407360
#define M_LOG10E    0.434294481903251827651
#define M_LN2       0.693147180559945309417
#define M_LN10      2.302585092994045684018
#define M_PI        3.141592653589793238463
#define M_PI_2      1.570796326794896619231
#define M_PI_4      0.785398163397448309616
#define M_SQRT2     1.414213562373095048802
#define HUGE_VAL    (_u2d(0x7FF0000000000000ULL))
#define INFINITY    HUGE_VAL
#define NAN         (_u2d(0x7FF8000000000000ULL))

/* ── Classification ──────────────────────────────────────────── */
static inline int isinf(double x) {
    return (_d2u(x) & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}
static inline int isnan(double x) {
    unsigned long long u = _d2u(x) & 0x7FFFFFFFFFFFFFFFULL;
    return u > 0x7FF0000000000000ULL;
}
static inline int isfinite(double x) { return !isinf(x) && !isnan(x); }
static inline int isnormal(double x) {
    unsigned long long e = (_d2u(x) >> 52) & 0x7FF;
    return e != 0 && e != 0x7FF;
}

/* ── fabs / copysign ─────────────────────────────────────────── */
static inline double fabs(double x) {
    return _u2d(_d2u(x) & 0x7FFFFFFFFFFFFFFFULL);
}
static inline double copysign(double x, double y) {
    return _u2d((_d2u(x) & 0x7FFFFFFFFFFFFFFFULL) |
                (_d2u(y) & 0x8000000000000000ULL));
}

/* ── floor / ceil / trunc / round ────────────────────────────── */
static inline double trunc(double x) { return (double)(long long)x; }
static inline double floor(double x) {
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}
static inline double ceil(double x) {
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}
static inline double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}
static inline double rint(double x)  { return round(x); }
static inline double nearbyint(double x) { return round(x); }
static inline long lround(double x)  { return (long)round(x); }
static inline long long llround(double x) { return (long long)round(x); }

/* ── fmod ────────────────────────────────────────────────────── */
static inline double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    double q = trunc(x / y);
    return x - q * y;
}

/* ── frexp / ldexp / modf ────────────────────────────────────── */
static inline double ldexp(double x, int exp) {
    /* multiply x by 2^exp */
    union { double d; unsigned long long u; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0 || e == 0x7FF) return x;
    e += exp;
    if (e <= 0)   return copysign(0.0, x);
    if (e >= 0x7FF) return copysign(INFINITY, x);
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)e << 52);
    return v.d;
}
static inline double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    union { double d; unsigned long long u; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF) - 1022;
    *exp = e;
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | (0x3FEULL << 52);
    return v.d;
}
static inline double modf(double x, double *iptr) {
    double t = trunc(x);
    *iptr = t;
    return x - t;
}

/* ── sqrt — Newton-Raphson ───────────────────────────────────── */
static inline double sqrt(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0 || isinf(x)) return x;
    /* initial guess via bit hack */
    union { double d; unsigned long long u; } v;
    v.d = x;
    v.u = (v.u >> 1) + (0x3FF0000000000000ULL >> 1);
    double g = v.d;
    /* 4 Newton iterations — enough for full double precision */
    g = 0.5 * (g + x / g);
    g = 0.5 * (g + x / g);
    g = 0.5 * (g + x / g);
    g = 0.5 * (g + x / g);
    return g;
}

/* ── exp — Taylor series with range reduction ────────────────── */
static inline double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.8) return INFINITY;
    if (x < -745.1) return 0.0;
    /* range reduction: e^x = 2^k * e^r, r in [-ln2/2, ln2/2] */
    double k = floor(x * M_LOG2E + 0.5);
    double r = x - k * M_LN2;
    /* minimax polynomial for e^r */
    double p = 1.0 + r*(1.0 + r*(0.5 + r*(1.0/6.0 + r*(1.0/24.0 +
               r*(1.0/120.0 + r*(1.0/720.0 + r*(1.0/5040.0)))))));
    return ldexp(p, (int)k);
}
static inline double exp2(double x)  { return exp(x * M_LN2); }
static inline double exp10(double x) { return exp(x * M_LN10); }
static inline double expm1(double x) { return exp(x) - 1.0; }

/* ── log — range reduction + rational approx ─────────────────── */
static inline double log(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0) return -INFINITY;
    if (isinf(x)) return INFINITY;
    /* extract exponent and mantissa in [1, 2) */
    int e;
    double m = frexp(x, &e);   /* x = m * 2^e, m in [0.5, 1) */
    if (m < M_SQRT2 * 0.5) { m *= 2.0; e--; }
    /* log(m) for m in [sqrt(2)/2, sqrt(2)] using Padé approx */
    double u = (m - 1.0) / (m + 1.0);
    double u2 = u * u;
    double p = u * (2.0 + u2 * (2.0/3.0 + u2 * (2.0/5.0 +
               u2 * (2.0/7.0 + u2 * (2.0/9.0 + u2 * (2.0/11.0))))));
    return p + (double)e * M_LN2;
}
static inline double log2(double x)  { return log(x) * M_LOG2E; }
static inline double log10(double x) { return log(x) * M_LOG10E; }
static inline double log1p(double x) { return log(1.0 + x); }

/* ── pow ─────────────────────────────────────────────────────── */
static inline double pow(double base, double exp_) {
    if (exp_ == 0.0) return 1.0;
    if (base == 1.0) return 1.0;
    if (isnan(base) || isnan(exp_)) return NAN;
    if (base == 0.0) return (exp_ > 0.0) ? 0.0 : INFINITY;
    /* integer exponent fast path */
    if (exp_ == (double)(long long)exp_ && fabs(exp_) < 1e15) {
        long long n = (long long)exp_;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1.0, b = base;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1.0 / r : r;
    }
    if (base < 0.0) return NAN;  /* non-integer power of negative */
    return exp(exp_ * log(base));
}

/* ── sin / cos — Bhaskara-inspired minimax on [-pi/4, pi/4] ──── */

/* sin on [-pi/2, pi/2] via polynomial */
static inline double _sin_core(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 +
           x2 * (-1.0/5040.0 + x2 * (1.0/362880.0 +
           x2 * (-1.0/39916800.0))))));
}
/* cos on [-pi/2, pi/2] via polynomial */
static inline double _cos_core(double x) {
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24.0 + x2 * (-1.0/720.0 +
           x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0)))));
}

static inline double sin(double x) {
    /* range reduction to [-pi/4, pi/4] via quadrant */
    double pi2 = 2.0 * M_PI;
    x = fmod(x, pi2);
    if (x > M_PI)  x -= pi2;
    if (x < -M_PI) x += pi2;
    int neg = 0;
    if (x < 0.0) { x = -x; neg = 1; }
    double r;
    if (x <= M_PI_4)            r = _sin_core(x);
    else if (x <= M_PI_2)       r = _cos_core(M_PI_2 - x);
    else if (x <= 3*M_PI_4)     r = _cos_core(x - M_PI_2);
    else if (x <= M_PI)         r = _sin_core(M_PI - x);
    else                        r = 0.0;
    return neg ? -r : r;
}

static inline double cos(double x) {
    return sin(x + M_PI_2);
}

static inline double tan(double x) {
    double c = cos(x);
    return (c == 0.0) ? INFINITY : sin(x) / c;
}

/* ── asin / acos / atan ──────────────────────────────────────── */
static inline double atan(double x) {
    /* range reduce to [0,1] then use minimax */
    int neg = 0, inv = 0;
    if (x < 0.0) { x = -x; neg = 1; }
    if (x > 1.0) { x = 1.0 / x; inv = 1; }
    double x2 = x * x;
    double p = x * (1.0 + x2 * (-1.0/3.0 + x2 * (1.0/5.0 +
               x2 * (-1.0/7.0 + x2 * (1.0/9.0 + x2 * (-1.0/11.0 +
               x2 * (1.0/13.0)))))));
    if (inv) p = M_PI_2 - p;
    return neg ? -p : p;
}
static inline double atan2(double y, double x) {
    if (x == 0.0) {
        if (y == 0.0) return 0.0;
        return (y > 0.0) ? M_PI_2 : -M_PI_2;
    }
    double a = atan(y / x);
    if (x < 0.0) return (y >= 0.0) ? a + M_PI : a - M_PI;
    return a;
}
static inline double asin(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0)  return  M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan2(x, sqrt(1.0 - x * x));
}
static inline double acos(double x) {
    return M_PI_2 - asin(x);
}

/* ── sinh / cosh / tanh ──────────────────────────────────────── */
static inline double sinh(double x) {
    double e = exp(x);
    return (e - 1.0/e) * 0.5;
}
static inline double cosh(double x) {
    double e = exp(x);
    return (e + 1.0/e) * 0.5;
}
static inline double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}
static inline double asinh(double x) { return log(x + sqrt(x*x + 1.0)); }
static inline double acosh(double x) {
    if (x < 1.0) return NAN;
    return log(x + sqrt(x*x - 1.0));
}
static inline double atanh(double x) {
    if (fabs(x) >= 1.0) return (fabs(x) == 1.0) ? INFINITY : NAN;
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

/* ── hypot ───────────────────────────────────────────────────── */
static inline double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

/* ── cbrt ────────────────────────────────────────────────────── */
static inline double cbrt(double x) {
    if (x == 0.0) return 0.0;
    int neg = x < 0.0;
    if (neg) x = -x;
    double g = pow(x, 1.0/3.0);
    /* one Newton step */
    g = (2.0*g + x/(g*g)) / 3.0;
    return neg ? -g : g;
}

/* ── min / max / clamp helpers ───────────────────────────────── */
static inline double fmin(double a, double b) { return (a < b) ? a : b; }
static inline double fmax(double a, double b) { return (a > b) ? a : b; }
static inline double fdim(double a, double b) { return (a > b) ? a-b : 0.0; }
static inline double fma(double a, double b, double c) { return a*b + c; }

/* ── scalbn / scalbln ────────────────────────────────────────── */
static inline double scalbn(double x, int n)   { return ldexp(x, n); }
static inline double scalbln(double x, long n) { return ldexp(x, (int)n); }

/* ── logb / ilogb ────────────────────────────────────────────── */
static inline double logb(double x) {
    int e; frexp(x, &e); return (double)(e - 1);
}
static inline int ilogb(double x) {
    int e; frexp(x, &e); return e - 1;
}

/* ── remainder / remquo ──────────────────────────────────────── */
static inline double remainder(double x, double y) {
    double n = round(x / y);
    return x - n * y;
}
static inline double remquo(double x, double y, int *q) {
    double n = round(x / y);
    *q = (int)n;
    return x - n * y;
}

/* ── float variants (f suffix) — just cast to/from double ─────── */
static inline float sinf(float x)   { return (float)sin((double)x); }
static inline float cosf(float x)   { return (float)cos((double)x); }
static inline float tanf(float x)   { return (float)tan((double)x); }
static inline float asinf(float x)  { return (float)asin((double)x); }
static inline float acosf(float x)  { return (float)acos((double)x); }
static inline float atanf(float x)  { return (float)atan((double)x); }
static inline float atan2f(float y, float x) { return (float)atan2((double)y,(double)x); }
static inline float sqrtf(float x)  { return (float)sqrt((double)x); }
static inline float expf(float x)   { return (float)exp((double)x); }
static inline float exp2f(float x)  { return (float)exp2((double)x); }
static inline float logf(float x)   { return (float)log((double)x); }
static inline float log2f(float x)  { return (float)log2((double)x); }
static inline float log10f(float x) { return (float)log10((double)x); }
static inline float powf(float b, float e) { return (float)pow((double)b,(double)e); }
static inline float fabsf(float x)  { return (float)fabs((double)x); }
static inline float floorf(float x) { return (float)floor((double)x); }
static inline float ceilf(float x)  { return (float)ceil((double)x); }
static inline float roundf(float x) { return (float)round((double)x); }
static inline float truncf(float x) { return (float)trunc((double)x); }
static inline float fmodf(float x, float y) { return (float)fmod((double)x,(double)y); }
static inline float hypotf(float x, float y){ return (float)hypot((double)x,(double)y); }
static inline float sinhf(float x)  { return (float)sinh((double)x); }
static inline float coshf(float x)  { return (float)cosh((double)x); }
static inline float tanhf(float x)  { return (float)tanh((double)x); }
static inline float cbrtf(float x)  { return (float)cbrt((double)x); }
static inline float fminf(float a, float b) { return (a<b)?a:b; }
static inline float fmaxf(float a, float b) { return (a>b)?a:b; }
static inline float copysignf(float x, float y) {
    return (float)copysign((double)x,(double)y);
}
static inline float scalbnf(float x, int n) { return (float)ldexp((double)x,n); }
static inline float remainderf(float x, float y) {
    return (float)remainder((double)x,(double)y);
}
static inline int isinff(float x)  { return isinf((double)x); }
static inline int isnanf(float x)  { return isnan((double)x); }
