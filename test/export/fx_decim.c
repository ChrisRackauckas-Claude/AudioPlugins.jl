#include "fx_decim.h"

#include <math.h>

fx_decim_out fx_decim_step(double u, FxDecimPars *pars, fx_decim_mem *self) {
    fx_decim_out out;
    int64_t d = pars->divisor < 1 ? 1 : pars->divisor;
    out.has_y = (self->n % d) == 0;
    out.y = out.has_y ? pars->gain * u : NAN;
    self->n += 1;
    return out;
}

void fx_decim_reset(fx_decim_mem *self) { self->n = 0; }
