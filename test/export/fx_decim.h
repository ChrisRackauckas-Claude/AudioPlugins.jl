/* fx_decim.h -- a sample-and-hold decimator whose output is on a slower
 * clock than the input: every `divisor`-th sample the output is present
 * (has_y) and carries gain * u; in between it is absent, and y holds NaN
 * so that a wrapper reading it without looking at the flag cannot pass. */
#ifndef FX_DECIM_H
#define FX_DECIM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FxDecimPars FxDecimPars;
struct FxDecimPars { double gain; int64_t divisor; };

typedef struct { int64_t n; } fx_decim_mem;
typedef struct { double y; bool has_y; } fx_decim_out;

fx_decim_out fx_decim_step(double u, FxDecimPars *pars, fx_decim_mem *self);
void fx_decim_reset(fx_decim_mem *self);

#endif
