/* fx_eq.h -- an RBJ peaking equaliser as a per-sample step function, with
 * the sample rate as an ordinary parameter and its coefficients recomputed
 * every sample (as generated code without a parameter-change hook does).
 * Its state is a direct-form-I biquad: the fixture whose continuity across
 * blocks the tests check bitwise. */
#ifndef FX_EQ_H
#define FX_EQ_H

#include <stdbool.h>

typedef struct FxEqPars FxEqPars;
struct FxEqPars { double fs; double f0; double q; double gain_db; bool enabled; };

typedef struct { double x1, x2, y1, y2; } fx_eq_mem;
typedef struct { double y; } fx_eq_out;

fx_eq_out fx_eq_step(double u, FxEqPars *pars, fx_eq_mem *self);
void fx_eq_reset(fx_eq_mem *self);

#endif
