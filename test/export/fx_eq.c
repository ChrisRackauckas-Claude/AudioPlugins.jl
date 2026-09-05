#include "fx_eq.h"

#include <math.h>

fx_eq_out fx_eq_step(double u, FxEqPars *pars, fx_eq_mem *self) {
    fx_eq_out out;
    if (!pars->enabled) { out.y = u; return out; }
    double A = pow(10.0, pars->gain_db / 40.0);
    double w0 = 2.0 * M_PI * pars->f0 / pars->fs;
    double alpha = sin(w0) / (2.0 * pars->q);
    double cw = cos(w0);
    double b0 = 1.0 + alpha * A, b1 = -2.0 * cw, b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A, a1 = -2.0 * cw, a2 = 1.0 - alpha / A;
    double y = (b0 / a0) * u + (b1 / a0) * self->x1 + (b2 / a0) * self->x2
             - (a1 / a0) * self->y1 - (a2 / a0) * self->y2;
    self->x2 = self->x1; self->x1 = u;
    self->y2 = self->y1; self->y1 = y;
    out.y = y;
    return out;
}

void fx_eq_reset(fx_eq_mem *self) { self->x1 = self->x2 = self->y1 = self->y2 = 0.0; }
