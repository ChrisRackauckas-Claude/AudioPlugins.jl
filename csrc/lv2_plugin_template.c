/* lv2_plugin_template.c -- an LV2 plugin around a per-sample C step function.
 *
 * Not compilable as it stands: AudioPlugins.export_plugin renders it by
 * substituting every at-sign-delimited token from a PluginSpec, then
 * compiles the result under -Wall -Wextra -Werror. The step function it
 * wraps is compiled separately and only has to satisfy the ABI declared
 * in @HEADER@, the same one the CLAP template wraps:
 *
 *   @BASE@_out @BASE@_step(<inputs...>, @PARS@ *pars, @BASE@_mem *self);
 *   void       @BASE@_reset(@BASE@_mem *self);
 *
 * One `@BASE@_mem` per channel persists across run() calls; that is what
 * makes two consecutive blocks equal one continuous run. The host owns
 * the audio buffers and the step function only ever sees one sample.
 *
 * Port indices are the bundle's public contract, because an LV2 host
 * addresses a control port by index and this package's own host reports
 * that index as the parameter id. The Turtle written beside this binary
 * declares exactly the same layout:
 *
 *   0                  .. P-1        control input, one per descriptor
 *                                    parameter, in descriptor order
 *   P                  .. P+C-1      audio input,  one per channel
 *   P+C                .. P+2C-1     audio output, one per channel
 *   P+2C                             lv2:latency control output, present
 *                                    only when the descriptor declares a
 *                                    non-zero latency
 *
 * The plugin requires no host feature. LV2 control ports are floats read
 * once per run(), so parameters arrive at float precision and change at
 * block boundaries rather than per sample.
 */

#include <lv2/core/lv2.h>
#include "@HEADER@"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef @PARS@ ap_pars_t;
typedef @BASE@_mem ap_mem_t;
typedef @BASE@_out ap_out_t;

#define AP_URI      @URI@
#define AP_CHANNELS @CHANNELS@
#define AP_N_PARAMS @N_PARAMS@
#define AP_LATENCY  @LATENCY@

#define AP_PORT_AUDIO_IN  (AP_N_PARAMS)
#define AP_PORT_AUDIO_OUT (AP_N_PARAMS + AP_CHANNELS)
#define AP_PORT_LATENCY   (AP_N_PARAMS + 2 * AP_CHANNELS)

/* ---------------------------------------------------------------- *
 * Parameter table and instance
 * ---------------------------------------------------------------- */

typedef struct {
    double min, max, def;
    bool   stepped;
} ap_param_info_t;

/* One trailing sentinel so a plugin with no parameters still has an array. */
static const ap_param_info_t PARAM_INFO[AP_N_PARAMS + 1] = {
    @PARAM_INFO@
    { 0.0, 0.0, 0.0, false },
};

typedef struct {
    ap_pars_t    pars;
    ap_mem_t     mem[AP_CHANNELS];
    double       held[AP_CHANNELS];      /* last present output, for a sub-clock output */
    double       values[AP_N_PARAMS + 1];
    const float *ctrl[AP_N_PARAMS + 1];
    const float *in[AP_CHANNELS];
    float       *out[AP_CHANNELS];
    float       *latency;
} inst_t;

static double round_half_away(double v) {
    return (double)(long long)(v + (v >= 0.0 ? 0.5 : -0.5));
}

static void set_param(inst_t *s, int index, double v) {
    const ap_param_info_t *info = &PARAM_INFO[index];
    if (v < info->min) v = info->min;
    if (v > info->max) v = info->max;
    if (info->stepped) v = round_half_away(v);
    s->values[index] = v;
    switch (index) {
    @PARAM_APPLY@
    default: break;
    }
}

static void reset_state(inst_t *s) {
    for (int c = 0; c < AP_CHANNELS; c++) @BASE@_reset(&s->mem[c]);
    memset(s->held, 0, sizeof s->held);
}

/* ---------------------------------------------------------------- *
 * LV2_Descriptor entry points
 * ---------------------------------------------------------------- */

static LV2_Handle instantiate(const LV2_Descriptor *desc, double rate,
                              const char *bundle, const LV2_Feature *const *features) {
    (void)desc; (void)bundle; (void)features;   /* no feature is required */
    inst_t *s = (inst_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    @CONSTANTS@
    @ON_RATE@
    for (int i = 0; i < AP_N_PARAMS; i++) set_param(s, i, PARAM_INFO[i].def);
    reset_state(s);
    return (LV2_Handle)s;
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data) {
    inst_t *s = (inst_t *)handle;
    if (port < (uint32_t)AP_PORT_AUDIO_IN) {
        s->ctrl[port] = (const float *)data;
    } else if (port < (uint32_t)AP_PORT_AUDIO_OUT) {
        s->in[port - AP_PORT_AUDIO_IN] = (const float *)data;
    } else if (port < (uint32_t)AP_PORT_LATENCY) {
        s->out[port - AP_PORT_AUDIO_OUT] = (float *)data;
    } else if (port == (uint32_t)AP_PORT_LATENCY) {
        s->latency = (float *)data;         /* only declared when AP_LATENCY > 0 */
    }
}

static void activate(LV2_Handle handle) {
    inst_t *s = (inst_t *)handle;
    reset_state(s);
    if (s->latency) *s->latency = (float)AP_LATENCY;
}

static void run(LV2_Handle handle, uint32_t n) {
    inst_t *s = (inst_t *)handle;
    /* Control ports are per-block in LV2: one read here is the whole of
     * this block's automation. */
    for (int i = 0; i < AP_N_PARAMS; i++)
        if (s->ctrl[i]) set_param(s, i, (double)*s->ctrl[i]);
    if (s->latency) *s->latency = (float)AP_LATENCY;
    for (int c = 0; c < AP_CHANNELS; c++) {
        const float *src = s->in[c];
        float *dst = s->out[c];
        if (!src || !dst) continue;         /* an unconnected channel does not advance */
        for (uint32_t i = 0; i < n; i++) {
            double x = (double)src[i];
            ap_out_t o = @BASE@_step(@STEP_ARGS@ &s->pars, &s->mem[c]);
            @OUTPUT_READ@
            dst[i] = (float)y;
        }
    }
}

static void deactivate(LV2_Handle handle) { (void)handle; }

static void cleanup(LV2_Handle handle) { free(handle); }

static const void *extension_data(const char *uri) { (void)uri; return NULL; }

static const LV2_Descriptor DESC = {
    AP_URI, instantiate, connect_port, activate,
    run, deactivate, cleanup, extension_data,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index == 0) ? &DESC : NULL;
}
