/* ap_test_many_lv2.c -- an LV2 bundle of more plugins than any fixed-size
 * descriptor cache in the host, the LV2 counterpart of ap_test_many.c. A
 * search path holding hundreds of plugins is ordinary (the LSP collection
 * alone is 198, and a real LV2_PATH adds the system directories), so a scan
 * that stops early has to be visible in a test.
 *
 * Every plugin is a gain with the same port layout as ap_test_lv2.c's
 *   0 gain (ctrl in), 1 in, 2 out
 * scaled by its own index + 1, so hosting `urn:audioplugins:test:many:N`
 * and measuring the output says which descriptor was instantiated.
 *
 * Port descriptions are generated next to the bundle rather than kept as a
 * checked-in .ttl: they are AP_MANY_LV2_N copies of one block.
 */
#include "../../csrc/vendor/lv2/core/lv2.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef AP_MANY_LV2_N
#  define AP_MANY_LV2_N 300
#endif

#define AP_MANY_LV2_URI_LEN 40

typedef struct {
    const float *gain, *in;
    float       *out;
    float        scale;
} inst_t;

static char           URIS[AP_MANY_LV2_N][AP_MANY_LV2_URI_LEN];
static LV2_Descriptor DESCS[AP_MANY_LV2_N];

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f) {
    (void)rate; (void)bundle; (void)f;
    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (s) s->scale = (float)(d - DESCS) + 1.0f;
    return s;
}
static void cleanup(LV2_Handle h) { free(h); }
static const void *extension_data(const char *uri) { (void)uri; return NULL; }
static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static void connect_port(LV2_Handle h, uint32_t port, void *data) {
    inst_t *s = (inst_t *)h;
    switch (port) {
    case 0: s->gain = (const float *)data; break;
    case 1: s->in   = (const float *)data; break;
    case 2: s->out  = (float *)data; break;
    default: break;
    }
}

static void run(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    if (!s->in || !s->out) return;
    float g = (s->gain ? *s->gain : 1.0f) * s->scale;
    for (uint32_t i = 0; i < n; i++) s->out[i] = s->in[i] * g;
}

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    if (index >= AP_MANY_LV2_N) return NULL;
    if (!DESCS[0].URI)
        for (uint32_t i = 0; i < AP_MANY_LV2_N; i++) {
            snprintf(URIS[i], sizeof URIS[i], "urn:audioplugins:test:many:%u", i);
            DESCS[i].URI            = URIS[i];
            DESCS[i].instantiate    = instantiate;
            DESCS[i].connect_port   = connect_port;
            DESCS[i].activate       = activate;
            DESCS[i].run            = run;
            DESCS[i].deactivate     = deactivate;
            DESCS[i].cleanup        = cleanup;
            DESCS[i].extension_data = extension_data;
        }
    return &DESCS[index];
}
