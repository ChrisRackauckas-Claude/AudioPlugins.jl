/* ap_test_sidechain.c -- a CLAP bundle whose plugins declare an audio
 * layout that is not "one bus of `channels` channels each way": a mono
 * main input plus a mono sidechain input, and one output. It exists to
 * catch a host that never reads clap.audio-ports: such a host hands every
 * plugin its own one-bus layout, and these refuse that outright.
 *
 *   ap.sidechain         in {main 1ch, sidechain 1ch}, out {main 1ch}
 *                        out = in * sidechain
 *   ap.sidechain.stereo  in {main 1ch, sidechain 1ch}, out {main 2ch}
 *                        out = (in * sidechain, sidechain)
 *
 * The mono one is the ZamComp layout -- two mono inputs, one mono output --
 * on which the first version of this host passed channel_count = 2 on the
 * single output bus, two past what the plugin declared and past the array
 * it indexes by channel. The stereo one makes the channel map itself
 * checkable: its second output is a copy of whatever arrived on the
 * sidechain, so a test can see which host channel the host fed it.
 *
 *   cc -O2 -fPIC -shared -o ap_sidechain.clap test/plugins/ap_test_sidechain.c
 */

#include "../../csrc/vendor/clap/clap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- *
 * Descriptors
 * ---------------------------------------------------------------- */

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t DESC_SIDECHAIN = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.sidechain",
    .name = "AudioPlugins Test Sidechain",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0",
    .description = "out = in * sidechain (mono out)",
    .features = FEATURES,
};
static const clap_plugin_descriptor_t DESC_SIDECHAIN_STEREO = {
    .clap_version = CLAP_VERSION_INIT, .id = "ap.sidechain.stereo",
    .name = "AudioPlugins Test Sidechain Stereo",
    .vendor = "JuliaHub", .url = "", .manual_url = "", .support_url = "",
    .version = "0.1.0",
    .description = "out = (in * sidechain, sidechain) (stereo out)",
    .features = FEATURES,
};

/* ---------------------------------------------------------------- *
 * Instance state
 * ---------------------------------------------------------------- */

typedef enum { KIND_MONO_OUT, KIND_STEREO_OUT } kind_t;

typedef struct {
    clap_plugin_t plugin;      /* must be first: the host holds this */
    kind_t kind;
} inst_t;

/* ---------------------------------------------------------------- *
 * clap.audio-ports -- what the plugin declares, which is the whole
 * point of the fixture: two mono inputs and one output.
 * ---------------------------------------------------------------- */

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) {
    (void)p;
    return is_input ? 2u : 1u;
}

static bool ports_get(const clap_plugin_t *p, uint32_t index, bool is_input,
                      clap_audio_port_info_t *info) {
    inst_t *s = p->plugin_data;
    memset(info, 0, sizeof *info);
    if (is_input) {
        if (index == 0) {
            info->id = 0;
            snprintf(info->name, sizeof info->name, "%s", "In");
            info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        } else if (index == 1) {
            info->id = 1;
            snprintf(info->name, sizeof info->name, "%s", "Sidechain");
            info->flags = 0;
        } else {
            return false;
        }
    } else {
        if (index != 0) return false;
        info->id = 0;
        snprintf(info->name, sizeof info->name, "%s", "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = (s->kind == KIND_STEREO_OUT) ? 2 : 1;
        info->port_type = (s->kind == KIND_STEREO_OUT) ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }
    info->channel_count = 1;
    info->port_type = CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t PORTS_EXT = { .count = ports_count, .get = ports_get };

/* ---------------------------------------------------------------- *
 * Plugin lifecycle
 * ---------------------------------------------------------------- */

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
    uint32_t want_out = (s->kind == KIND_STEREO_OUT) ? 2u : 1u;

    /* The layout the plugin declared is the only one it accepts: a host
     * that never asked sends one bus of `channels` channels each way, and
     * either count being wrong means this is not the audio the contract
     * described. Refusing loudly is the fixture -- a plugin that trusted
     * the count instead would be writing past its own tables. */
    int bad = !pr || pr->audio_inputs_count != 2 || pr->audio_outputs_count != 1;
    if (!bad)
        bad = pr->audio_inputs[0].channel_count != 1 ||
              pr->audio_inputs[1].channel_count != 1 ||
              pr->audio_outputs[0].channel_count != want_out;
    if (bad) {
        fprintf(stderr,
                "%s: received layout is not the declared one: "
                "inputs_count=%u outputs_count=%u (want 2 and 1)\n",
                p->desc->id,
                pr ? pr->audio_inputs_count : 0,
                pr ? pr->audio_outputs_count : 0);
        return CLAP_PROCESS_ERROR;
    }

    const clap_audio_buffer_t *in = pr->audio_inputs;
    clap_audio_buffer_t *out = pr->audio_outputs;
    if (!in[0].data32 || !in[1].data32 || !out[0].data32) return CLAP_PROCESS_ERROR;

    uint32_t n = pr->frames_count;
    for (uint32_t i = 0; i < n; i++) {
        float side = in[1].data32[0][i];
        out[0].data32[0][i] = in[0].data32[0][i] * side;
        if (s->kind == KIND_STEREO_OUT) out[0].data32[1][i] = side;
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS_EXT;
    return NULL;
}

static const clap_plugin_t *make(kind_t kind, const clap_plugin_descriptor_t *desc,
                                 const clap_host_t *host) {
    (void)host;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->kind = kind;
    s->plugin.desc = desc;
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

static const clap_plugin_descriptor_t *const DESCS[] = {
    &DESC_SIDECHAIN, &DESC_SIDECHAIN_STEREO,
};
#define N_DESCS (sizeof DESCS / sizeof DESCS[0])

static uint32_t factory_count(const clap_plugin_factory_t *f) { (void)f; return N_DESCS; }

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index < N_DESCS) ? DESCS[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    if (!id) return NULL;
    if (strcmp(id, DESC_SIDECHAIN.id) == 0)
        return make(KIND_MONO_OUT, &DESC_SIDECHAIN, host);
    if (strcmp(id, DESC_SIDECHAIN_STEREO.id) == 0)
        return make(KIND_STEREO_OUT, &DESC_SIDECHAIN_STEREO, host);
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
