/* ap_test_lv2.c -- a minimal LV2 gain plugin, ours, for testing the host.
 * Ports: 0 control (gain), 1 audio in, 2 audio out. */
#include "../../csrc/vendor/lv2/core/lv2.h"
#include <stdlib.h>

#define URI "urn:juliahub:dyad-test-gain"

typedef struct { const float *gain, *in; float *out; } inst_t;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f) {
    (void)d; (void)rate; (void)bundle; (void)f;
    return calloc(1, sizeof(inst_t));
}
static void connect_port(LV2_Handle h, uint32_t port, void *data) {
    inst_t *s = (inst_t *)h;
    switch (port) {
    case 0: s->gain = (const float *)data; break;
    case 1: s->in   = (const float *)data; break;
    case 2: s->out  = (float *)data; break;
    default: break;
    }
}
static void activate(LV2_Handle h) { (void)h; }
static void run(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    if (!s->in || !s->out) return;
    float g = s->gain ? *s->gain : 1.0f;
    for (uint32_t i = 0; i < n; i++) s->out[i] = s->in[i] * g;
}
static void deactivate(LV2_Handle h) { (void)h; }
static void cleanup(LV2_Handle h) { free(h); }
static const void *extension_data(const char *uri) { (void)uri; return NULL; }

static const LV2_Descriptor DESC = {
    URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index == 0) ? &DESC : NULL;
}
