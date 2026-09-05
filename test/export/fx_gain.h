/* fx_gain.h -- a hand-written step function in the shape a fixed-step code
 * generator emits: a named parameter struct passed by typed pointer, a
 * per-instance state struct, a result struct, and <base>_step/<base>_reset.
 * Nothing generated this; it is the fixture that proves the exporter needs
 * no generator. */
#ifndef FX_GAIN_H
#define FX_GAIN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FxGainPars FxGainPars;
struct FxGainPars { double gain; bool bypass; int64_t unused; };

typedef struct { int64_t ticks; } fx_gain_mem;
typedef struct { double y; } fx_gain_out;

fx_gain_out fx_gain_step(double u, bool clock1, FxGainPars *pars, fx_gain_mem *self);
void fx_gain_reset(fx_gain_mem *self);

#endif
