/* fx_look.h -- a lookahead delay as a step function: the output is the
 * input FX_LOOK_DELAY samples ago, scaled. The plugin does not hide that
 * delay, it reports it, so this is the fixture that makes a descriptor's
 * declared latency observable from a host. */
#ifndef FX_LOOK_H
#define FX_LOOK_H

#include <stdint.h>

#define FX_LOOK_DELAY 16

typedef struct FxLookPars FxLookPars;
struct FxLookPars { double gain; };

typedef struct { double buf[FX_LOOK_DELAY]; int64_t pos; } fx_look_mem;
typedef struct { double y; } fx_look_out;

fx_look_out fx_look_step(double u, FxLookPars *pars, fx_look_mem *self);
void fx_look_reset(fx_look_mem *self);

#endif
