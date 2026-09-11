/* ap_test_many.c -- a CLAP bundle with more plugins than any fixed-size
 * cache would hold, for probing the host's discovery path rather than its
 * arithmetic (test/plugins/ap_test_plugins.c is where the arithmetic is).
 *
 * One CLAP module can carry hundreds of plugins -- the collections worth
 * bundling as a JLL run to several hundred -- so a host whose descriptor
 * cache stops at some round number reports such a bundle as smaller than
 * it is, which from the outside is indistinguishable from a bundle that
 * really is that small. This fixture is the case that catches it.
 *
 * Plugin 0 is the descriptor probe: every optional field set to something
 * recognisable, and a deliberately over-long feature list, so a reader can
 * check both that the fields arrive and that too many features truncate
 * rather than overrun. Plugins 1..N-1 are identical passthroughs whose
 * only job is to exist and be named distinctly.
 *
 *   cc -O2 -fPIC -shared -o ap_many.clap test/plugins/ap_test_many.c
 */

#include "../../csrc/vendor/clap/clap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Comfortably past the 32 the cache used to stop at, and past any round
 * number a later regression is likely to pick. Overridable so a probe can
 * push it further without editing the fixture. */
#ifndef AP_MANY_N
#  define AP_MANY_N 100
#endif

#define AP_MANY_ID_LEN 32
#define AP_MANY_NAME_LEN 48

/* ---------------------------------------------------------------- *
 * Descriptors, built once at entry->init
 *
 * The ids and names are owned here rather than formatted on demand into a
 * shared buffer: a descriptor's strings must outlive every call the host
 * makes, and a host that returned borrowed pointers would hide that.
 * ---------------------------------------------------------------- */

/* More features than the host caches, so truncation is exercised. */
static const char *const FEATURES_RICH[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
    "f02", "f03", "f04", "f05", "f06", "f07", "f08", "f09",
    "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17",
    NULL,
};

static const char *const FEATURES_PLAIN[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL,
};

static char IDS[AP_MANY_N][AP_MANY_ID_LEN];
static char NAMES[AP_MANY_N][AP_MANY_NAME_LEN];
static clap_plugin_descriptor_t DESCS[AP_MANY_N];

static void build_descs(void) {
    for (int i = 0; i < AP_MANY_N; i++) {
        snprintf(IDS[i], sizeof IDS[i], "ap.many.%03d", i);
        snprintf(NAMES[i], sizeof NAMES[i], "AudioPlugins Many %03d", i);
        DESCS[i].clap_version = CLAP_VERSION;
        DESCS[i].id = IDS[i];
        DESCS[i].name = NAMES[i];
        DESCS[i].vendor = "";
        DESCS[i].url = "";
        DESCS[i].manual_url = "";
        DESCS[i].support_url = "";
        DESCS[i].version = "";
        DESCS[i].description = "";
        DESCS[i].features = FEATURES_PLAIN;
    }
    /* Plugin 0 carries the full descriptor, so one scan checks every field
     * and the empty-string answer for the unset ones on its neighbours. */
    DESCS[0].vendor = "AudioPlugins Test Vendor";
    DESCS[0].url = "https://example.invalid/many";
    DESCS[0].version = "4.5.6";
    DESCS[0].description = "every optional descriptor field, set";
    DESCS[0].features = FEATURES_RICH;
}

/* ---------------------------------------------------------------- *
 * The plugin: a passthrough, so that "found it" can be told apart from
 * "can actually open and run it".
 * ---------------------------------------------------------------- */

typedef struct {
    clap_plugin_t plugin;
} inst_t;

static bool plug_init(const clap_plugin_t *p) { (void)p; return true; }
static void plug_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool plug_activate(const clap_plugin_t *p, double sr, uint32_t lo, uint32_t hi) {
    (void)p; (void)sr; (void)lo; (void)hi; return true;
}
static void plug_deactivate(const clap_plugin_t *p) { (void)p; }
static bool plug_start(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop(const clap_plugin_t *p) { (void)p; }
static void plug_reset(const clap_plugin_t *p) { (void)p; }
static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }
static const void *plug_get_extension(const clap_plugin_t *p, const char *id) {
    (void)p; (void)id; return NULL;
}

static clap_process_status plug_process(const clap_plugin_t *p,
                                        const clap_process_t *pr) {
    (void)p;
    if (!pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;
    const clap_audio_buffer_t *in = &pr->audio_inputs[0];
    clap_audio_buffer_t *out = &pr->audio_outputs[0];
    for (uint32_t c = 0; c < out->channel_count; c++) {
        const float *src = (c < in->channel_count) ? in->data32[c] : NULL;
        float *dst = out->data32[c];
        for (uint32_t i = 0; i < pr->frames_count; i++)
            dst[i] = src ? src[i] : 0.0f;
    }
    return CLAP_PROCESS_CONTINUE;
}

static const clap_plugin_t *make(int index, const clap_host_t *host) {
    (void)host;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->plugin.desc = &DESCS[index];
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

static uint32_t factory_count(const clap_plugin_factory_t *f) {
    (void)f; return (uint32_t)AP_MANY_N;
}

static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f,
                                                    uint32_t index) {
    (void)f;
    return (index < (uint32_t)AP_MANY_N) ? &DESCS[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host, const char *id) {
    (void)f;
    if (!id) return NULL;
    for (int i = 0; i < AP_MANY_N; i++)
        if (strcmp(id, IDS[i]) == 0) return make(i, host);
    return NULL;
}

static const clap_plugin_factory_t FACTORY = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_desc,
    .create_plugin = factory_create,
};

static bool entry_init(const char *path) { (void)path; build_descs(); return true; }
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
