#include "fx_gain.h"

fx_gain_out fx_gain_step(double u, bool clock1, FxGainPars *pars, fx_gain_mem *self) {
    fx_gain_out out;
    if (clock1) self->ticks += 1;
    out.y = pars->bypass ? u : pars->gain * u;
    return out;
}

void fx_gain_reset(fx_gain_mem *self) { self->ticks = 0; }
