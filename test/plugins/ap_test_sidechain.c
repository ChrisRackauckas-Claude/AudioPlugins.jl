/* ap_test_sidechain.c -- CLAP fixtures declaring audio-port layouts that
 * are not "one bus of `channels` channels each way":
 *
 *   ap.sidechain         in {main 1ch, aux 1ch}  out {main 1ch}
 *                        out = main_in * aux_in      (ZamComp's layout)
 *   ap.sidechain.stereo  in {main 2ch, aux 1ch}  out {main 2ch}
 *                        out = (main_in[0], aux_in) (ZamCompX2's layout)
 *   ap.mixdown           in {main 2ch}           out {main 1ch}
 *                        out = (in[0] + in[1]) / 2
 *   ap.silent            in {main 1ch}           out {}
 *   ap.source            in {}                   out {main 1ch}, out = 0.25
 *
 *   cc -O2 -fPIC -shared -o ap_sidechain.clap test/plugins/ap_test_sidechain.c
 */

#include "../../csrc/vendor/clap/clap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { K_SIDECHAIN, K_SIDECHAIN_ST, K_MIXDOWN, K_SILENT, K_SOURCE } kind_t;

typedef struct {
    uint32_t n_in;   uint32_t in_cc[2];   /* ports, and channels per port */
    uint32_t n_out;  uint32_t out_cc[1];
} layout_t;

static const layout_t LAYOUT[] = {
    [K_SIDECHAIN]    = { 2, {1, 1}, 1, {1} },
    [K_SIDECHAIN_ST] = { 2, {2, 1}, 1, {2} },
    [K_MIXDOWN]      = { 1, {2, 0}, 1, {1} },
    [K_SILENT]       = { 1, {1, 0}, 0, {0} },
    [K_SOURCE]       = { 0, {0, 0}, 1, {1} },
};

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

#define MKDESC(ID, NAME, DESC) { \
    .clap_version = CLAP_VERSION_INIT, .id = ID, .name = NAME, \
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "", \
    .version = "0.1.0", .description = DESC, .features = FEATURES, }

static const clap_plugin_descriptor_t DESCS[] = {
    [K_SIDECHAIN]    = MKDESC("ap.sidechain", "AudioPlugins Test Sidechain",
                              "out = main_in * aux_in"),
    [K_SIDECHAIN_ST] = MKDESC("ap.sidechain.stereo", "AudioPlugins Test Sidechain Stereo",
                              "out = (main_in[0], aux_in)"),
    [K_MIXDOWN]      = MKDESC("ap.mixdown", "AudioPlugins Test Mixdown",
                              "out = (in[0] + in[1]) / 2"),
    [K_SILENT]       = MKDESC("ap.silent", "AudioPlugins Test Silent",
                              "an input, no outputs"),
    [K_SOURCE]       = MKDESC("ap.source", "AudioPlugins Test Source",
                              "no inputs, out = 0.25"),
};
#define N_DESCS (sizeof DESCS / sizeof DESCS[0])

typedef struct {
    clap_plugin_t plugin;      /* must be first: the host holds this */
    kind_t kind;
} inst_t;

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    const layout_t *l = &LAYOUT[((inst_t *)p->plugin_data)->kind];
    return is_input ? l->n_in : l->n_out;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    const layout_t *l = &LAYOUT[((inst_t *)p->plugin_data)->kind];
    uint32_t n = is_input ? l->n_in : l->n_out;
    if (index >= n) return false;
    memset(info, 0, sizeof *info);
    info->id = index;
    snprintf(info->name, sizeof info->name, "%s",
             is_input ? (index == 0 ? "In" : "Aux") : "Out");
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = is_input ? l->in_cc[index] : l->out_cc[index];
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t PORTS_EXT = { .count = ports_count, .get = ports_get };

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }
static void plug_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t minf, uint32_t maxf) {
    (void)p; (void)sr;
    return minf >= 1 && maxf <= 8192;
}
static void plug_deactivate(const clap_plugin_t *p) { (void)p; }
static bool plug_start(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop(const clap_plugin_t *p) { (void)p; }
static void plug_reset(const clap_plugin_t *p) { (void)p; }
static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
    inst_t *s = p->plugin_data;
    const layout_t *l = &LAYOUT[s->kind];

    /* Accept exactly the declared layout and nothing else. */
    int bad = !pr || pr->audio_inputs_count != l->n_in ||
              pr->audio_outputs_count != l->n_out;
    for (uint32_t i = 0; !bad && i < l->n_in; i++)
        bad = pr->audio_inputs[i].channel_count != l->in_cc[i];
    for (uint32_t i = 0; !bad && i < l->n_out; i++)
        bad = pr->audio_outputs[i].channel_count != l->out_cc[i];
    if (bad) {
        fprintf(stderr,
                "%s: received layout is not the declared one: "
                "inputs_count=%u outputs_count=%u (want %u and %u)\n",
                p->desc->id,
                pr ? pr->audio_inputs_count : 0,
                pr ? pr->audio_outputs_count : 0,
                l->n_in, l->n_out);
        return CLAP_PROCESS_ERROR;
    }

    uint32_t n = pr->frames_count;
    const clap_audio_buffer_t *in = pr->audio_inputs;
    clap_audio_buffer_t *out = pr->audio_outputs;
    switch (s->kind) {
    case K_SIDECHAIN:
        for (uint32_t i = 0; i < n; i++)
            out[0].data32[0][i] = in[0].data32[0][i] * in[1].data32[0][i];
        break;
    case K_SIDECHAIN_ST:
        for (uint32_t i = 0; i < n; i++) {
            out[0].data32[0][i] = in[0].data32[0][i];
            out[0].data32[1][i] = in[1].data32[0][i];
        }
        break;
    case K_MIXDOWN:
        for (uint32_t i = 0; i < n; i++)
            out[0].data32[0][i] = 0.5f * (in[0].data32[0][i] + in[0].data32[1][i]);
        break;
    case K_SILENT:
        break;
    case K_SOURCE:
        for (uint32_t i = 0; i < n; i++) out[0].data32[0][i] = 0.25f;
        break;
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    return NULL;
}

static const clap_plugin_t *make(kind_t kind, const clap_host_t *host) {
    (void)host;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->kind = kind;
    s->plugin.desc = &DESCS[kind];
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

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return N_DESCS; }

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index < N_DESCS) ? &DESCS[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    for (uint32_t k = 0; k < N_DESCS; k++)
        if (id && strcmp(id, DESCS[k].id) == 0) return make((kind_t)k, host);
    return NULL;
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
