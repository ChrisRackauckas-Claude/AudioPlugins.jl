/* clap_plugin_template.c -- a CLAP plugin around a per-sample C step function.
 *
 * Not compilable as it stands: AudioPlugins.export_plugin renders it by
 * substituting every at-sign-delimited token from a PluginSpec, then
 * compiles the result under -Wall -Wextra -Werror. The step function it
 * wraps is compiled separately and only has to satisfy the ABI declared
 * in @HEADER@:
 *
 *   @BASE@_out @BASE@_step(<inputs...>, @PARS@ *pars, @BASE@_mem *self);
 *   void       @BASE@_reset(@BASE@_mem *self);
 *
 * One `@BASE@_mem` per channel persists across process() calls; that is
 * what makes two consecutive blocks equal one continuous run. The host
 * owns the audio buffers and the step function only ever sees one sample.
 */

#include "clap/clap.h"
#include "@HEADER@"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef @PARS@ ap_pars_t;
typedef @BASE@_mem ap_mem_t;
typedef @BASE@_out ap_out_t;

#define AP_CHANNELS @CHANNELS@
#define AP_N_PARAMS @N_PARAMS@

/* ---------------------------------------------------------------- *
 * Descriptor and parameter table
 * ---------------------------------------------------------------- */

static const char *const FEATURES[] = { @FEATURES@ NULL };

static const clap_plugin_descriptor_t DESC = {
    .clap_version = CLAP_VERSION_INIT,
    .id = @ID@,
    .name = @NAME@,
    .vendor = @VENDOR@,
    .url = @URL@,
    .manual_url = "",
    .support_url = "",
    .version = @VERSION@,
    .description = @DESCRIPTION@,
    .features = FEATURES,
};

typedef struct {
    uint32_t    id;
    const char *name;
    double      min, max, def;
    uint32_t    flags;
} ap_param_info_t;

/* One trailing sentinel so a plugin with no parameters still has an array. */
static const ap_param_info_t PARAM_INFO[AP_N_PARAMS + 1] = {
    @PARAM_INFO@
    { 0u, NULL, 0.0, 0.0, 0.0, 0u },
};

/* ---------------------------------------------------------------- *
 * Instance
 * ---------------------------------------------------------------- */

typedef struct {
    clap_plugin_t plugin;               /* must be first: the host holds this */
    ap_pars_t     pars;
    ap_mem_t      mem[AP_CHANNELS];
    double        values[AP_N_PARAMS + 1];
    double        held[AP_CHANNELS];    /* last present output, for a sub-clock output */
    int           activated;
    int           processing;
} inst_t;

static int param_index(uint32_t id) {
    for (int i = 0; i < AP_N_PARAMS; i++)
        if (PARAM_INFO[i].id == id) return i;
    return -1;
}

static double round_half_away(double v) {
    return (double)(long long)(v + (v >= 0.0 ? 0.5 : -0.5));
}

static void set_param(inst_t *s, int index, double v) {
    const ap_param_info_t *info = &PARAM_INFO[index];
    if (v < info->min) v = info->min;
    if (v > info->max) v = info->max;
    if (info->flags & CLAP_PARAM_IS_STEPPED) v = round_half_away(v);
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

static void apply_event(inst_t *s, const clap_event_header_t *h) {
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (h->type != CLAP_EVENT_PARAM_VALUE) return;
    const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
    int i = param_index(e->param_id);
    if (i >= 0) set_param(s, i, e->value);
}

/* ---------------------------------------------------------------- *
 * clap.params
 * ---------------------------------------------------------------- */

static uint32_t params_count(const clap_plugin_t *p) {
    (void)p;
    return (uint32_t)AP_N_PARAMS;
}

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
    (void)p;
    if (index >= (uint32_t)AP_N_PARAMS) return false;
    const ap_param_info_t *pi = &PARAM_INFO[index];
    memset(info, 0, sizeof *info);
    info->id = pi->id;
    info->flags = pi->flags;
    info->cookie = NULL;
    snprintf(info->name, sizeof info->name, "%s", pi->name);
    info->module[0] = '\0';
    info->min_value = pi->min;
    info->max_value = pi->max;
    info->default_value = pi->def;
    return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *out) {
    const inst_t *s = p->plugin_data;
    int i = param_index(id);
    if (i < 0) return false;
    *out = s->values[i];
    return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double v,
                                 char *buf, uint32_t cap) {
    (void)p;
    if (param_index(id) < 0) return false;
    snprintf(buf, cap, "%.6g", v);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id,
                                 const char *txt, double *out) {
    (void)p;
    if (param_index(id) < 0 || !txt) return false;
    char *end = NULL;
    double v = strtod(txt, &end);
    if (end == txt) return false;
    *out = v;
    return true;
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                         const clap_output_events_t *out) {
    (void)out;
    if (!in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) apply_event(p->plugin_data, in->get(in, i));
}

static const clap_plugin_params_t PARAMS_EXT = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush,
};

/* ---------------------------------------------------------------- *
 * clap.audio-ports -- one main input and one main output
 * ---------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    (void)p; (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    (void)p;
    if (index != 0) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = AP_CHANNELS;
    info->port_type = @PORT_TYPE@;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t PORTS_EXT = { .count = ports_count, .get = ports_get };

/* ---------------------------------------------------------------- *
 * clap.state -- the parameter values, so a session reloads where it
 * left off. Little-endian on the wire whatever the machine, so a
 * project saved on one platform loads on another:
 *
 *   "APST" | u32 version | u32 count | count x (u32 id, f64 value)
 * ---------------------------------------------------------------- */

#define AP_STATE_VERSION 1u

static bool write_all(const clap_ostream_t *os, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        int64_t w = os->write(os, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(const clap_istream_t *is, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        int64_t r = is->read(is, p, n);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_u32(const clap_ostream_t *os, uint32_t v) {
    unsigned char b[4];
    for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
    return write_all(os, b, sizeof b);
}

static bool write_f64(const clap_ostream_t *os, double v) {
    uint64_t u;
    unsigned char b[8];
    memcpy(&u, &v, sizeof u);
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (8 * i));
    return write_all(os, b, sizeof b);
}

static bool read_u32(const clap_istream_t *is, uint32_t *v) {
    unsigned char b[4];
    if (!read_all(is, b, sizeof b)) return false;
    *v = 0;
    for (int i = 0; i < 4; i++) *v |= (uint32_t)b[i] << (8 * i);
    return true;
}

static bool read_f64(const clap_istream_t *is, double *v) {
    unsigned char b[8];
    uint64_t u = 0;
    if (!read_all(is, b, sizeof b)) return false;
    for (int i = 0; i < 8; i++) u |= (uint64_t)b[i] << (8 * i);
    memcpy(v, &u, sizeof u);
    return true;
}

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *os) {
    const inst_t *s = p->plugin_data;
    if (!write_all(os, "APST", 4) || !write_u32(os, AP_STATE_VERSION) ||
        !write_u32(os, (uint32_t)AP_N_PARAMS))
        return false;
    for (int i = 0; i < AP_N_PARAMS; i++)
        if (!write_u32(os, PARAM_INFO[i].id) || !write_f64(os, s->values[i])) return false;
    return true;
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *is) {
    inst_t *s = p->plugin_data;
    char magic[4];
    uint32_t version, count;
    if (!read_all(is, magic, sizeof magic) || memcmp(magic, "APST", 4) != 0) return false;
    if (!read_u32(is, &version) || version != AP_STATE_VERSION) return false;
    if (!read_u32(is, &count)) return false;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t id;
        double v;
        if (!read_u32(is, &id) || !read_f64(is, &v)) return false;
        int i = param_index(id);
        if (i >= 0) set_param(s, i, v);     /* an id this build lacks is skipped */
    }
    return true;
}

static const clap_plugin_state_t STATE_EXT = { .save = state_save, .load = state_load };

/* ---------------------------------------------------------------- *
 * Plugin lifecycle
 * ---------------------------------------------------------------- */

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }

static void plug_destroy(const clap_plugin_t *p) { free(p->plugin_data); }

static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t minf, uint32_t maxf) {
    inst_t *s = p->plugin_data;
    (void)minf; (void)maxf;             /* per-sample: any block size works */
    if (!(sr > 0.0)) return false;
    @ON_ACTIVATE@
    reset_state(s);
    s->activated = 1;
    return true;
}

static void plug_deactivate(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->activated = 0;
}

static bool plug_start(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->processing = 1;
    return true;
}

static void plug_stop(const clap_plugin_t *p) {
    ((inst_t *)p->plugin_data)->processing = 0;
}

static void plug_reset(const clap_plugin_t *p) { reset_state(p->plugin_data); }

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
    inst_t *s = p->plugin_data;
    if (!pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;

    const clap_audio_buffer_t *in = &pr->audio_inputs[0];
    clap_audio_buffer_t *out = &pr->audio_outputs[0];
    if (!(in->data32 || in->data64) || !(out->data32 || out->data64))
        return CLAP_PROCESS_ERROR;

    uint32_t nch = in->channel_count < out->channel_count
                 ? in->channel_count : out->channel_count;
    if (nch > AP_CHANNELS) nch = AP_CHANNELS;

    const clap_input_events_t *ev = pr->in_events;
    uint32_t n_ev = ev ? ev->size(ev) : 0u;
    uint32_t ev_i = 0;

    for (uint32_t i = 0; i < pr->frames_count; i++) {
        /* Sample-accurate automation: events are ordered by time. */
        for (; ev_i < n_ev; ev_i++) {
            const clap_event_header_t *h = ev->get(ev, ev_i);
            if (h && h->time > i) break;
            apply_event(s, h);
        }
        for (uint32_t c = 0; c < nch; c++) {
            double x = in->data32 ? (double)in->data32[c][i] : in->data64[c][i];
            ap_out_t o = @BASE@_step(@STEP_ARGS@ &s->pars, &s->mem[c]);
            @OUTPUT_READ@
            if (out->data32) out->data32[c][i] = (float)y;
            else             out->data64[c][i] = y;
        }
    }
    for (; ev_i < n_ev; ev_i++) apply_event(s, ev->get(ev, ev_i));
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &PARAMS_EXT;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &STATE_EXT;
    return NULL;
}

static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_t *make(const clap_host_t *host) {
    (void)host;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    @CONSTANTS@
    for (int i = 0; i < AP_N_PARAMS; i++) set_param(s, i, PARAM_INFO[i].def);
    reset_state(s);
    s->plugin.desc = &DESC;
    s->plugin.plugin_data = s;
    s->plugin.init = plug_init;
    s->plugin.destroy = plug_destroy;
    s->plugin.activate = plug_activate;
    s->plugin.deactivate = plug_deactivate;
    s->plugin.start_processing = plug_start;
    s->plugin.stop_processing = plug_stop;
    s->plugin.reset = plug_reset;
    s->plugin.process = plug_process;
    s->plugin.get_extension = plug_get_extension;
    s->plugin.on_main_thread = plug_on_main_thread;
    return &s->plugin;
}

/* ---------------------------------------------------------------- *
 * Factory and entry
 * ---------------------------------------------------------------- */

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return 1u; }

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index == 0) ? &DESC : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    if (!id || strcmp(id, DESC.id) != 0) return NULL;
    return make(host);
}

static const clap_plugin_factory_t FACTORY = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_desc,
    .create_plugin = factory_create,
};

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) { }

static const void *entry_get_factory(const char *id) {
    return (strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0) ? &FACTORY : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
