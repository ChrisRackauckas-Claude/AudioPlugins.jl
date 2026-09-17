#include "fx_look.h"

#include <string.h>

fx_look_out fx_look_step(double u, FxLookPars *pars, fx_look_mem *self) {
    fx_look_out out;
    out.y = pars->gain * self->buf[self->pos];
    self->buf[self->pos] = u;
    self->pos = (self->pos + 1) % FX_LOOK_DELAY;
    return out;
}

void fx_look_reset(fx_look_mem *self) {
    memset(self->buf, 0, sizeof self->buf);
    self->pos = 0;
}
