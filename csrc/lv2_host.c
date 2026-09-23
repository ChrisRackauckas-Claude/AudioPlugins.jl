/* lv2_host.c -- see lv2_host.h. Allocation-free after open, like
 * clap_host.c: lilv allocates during scan and open, which is driver-side.
 */

#include "lv2_host.h"
/* Through the include path rather than by relative path, so that lilv.h
 * (which includes <lv2/core/lv2.h> itself) and this file resolve to the
 * same copy: build with -I csrc/vendor to use the vendored headers. */
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/resize-port/resize-port.h>

#include <lilv/lilv.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1. Error reporting ---- */

static char ERR[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

const char *lv2_host_last_error(void) { return ERR; }

/* ---- 2. The host features we offer ----
 *
 * urid:map / urid:unmap because nearly every modern plugin requires
 * them; the two buf-size flags because they are true of this host. A
 * plugin that maps more than LV2_HOST_MAX_URIDS URIs gets 0 back, which
 * the spec allows for "cannot map".
 */

#define LV2_HOST_MAX_URIDS 1024

/* isSideChain postdates the vendored lv2.h, and port-groups ships no
 * header; the URIs are stable, so write them out. */
#define LV2_CORE__isSideChain  LV2_CORE_PREFIX "isSideChain"
#define LV2_PG_PREFIX          "http://lv2plug.in/ns/ext/port-groups#"
#define LV2_PG__group          LV2_PG_PREFIX "group"
#define LV2_PG__mainInput      LV2_PG_PREFIX "mainInput"
#define LV2_PG__mainOutput     LV2_PG_PREFIX "mainOutput"
#define LV2_PG__sideChainOf    LV2_PG_PREFIX "sideChainOf"

static char    *URIS[LV2_HOST_MAX_URIDS];
static uint32_t N_URIS;

static LV2_URID urid_map(LV2_URID_Map_Handle h, const char *uri) {
    (void)h;
    if (!uri) return 0;
    for (uint32_t i = 0; i < N_URIS; i++)
        if (strcmp(URIS[i], uri) == 0) return i + 1;
    if (N_URIS >= LV2_HOST_MAX_URIDS) return 0;
    URIS[N_URIS] = strdup(uri);
    if (!URIS[N_URIS]) return 0;
    return ++N_URIS;
}

static const char *urid_unmap(LV2_URID_Unmap_Handle h, LV2_URID urid) {
    (void)h;
    return (urid >= 1 && urid <= N_URIS) ? URIS[urid - 1] : NULL;
}

static LV2_URID_Map   MAP   = { NULL, urid_map };
static LV2_URID_Unmap UNMAP = { NULL, urid_unmap };

static const LV2_Feature F_MAP     = { LV2_URID__map,   &MAP };
static const LV2_Feature F_UNMAP   = { LV2_URID__unmap, &UNMAP };
static const LV2_Feature F_FIXED   = { LV2_BUF_SIZE__fixedBlockLength,   NULL };
static const LV2_Feature F_BOUNDED = { LV2_BUF_SIZE__boundedBlockLength, NULL };

static const LV2_Feature *const FEATURES[] = {
    &F_MAP, &F_UNMAP, &F_FIXED, &F_BOUNDED, NULL
};

static int feature_supported(const char *uri) {
    for (const LV2_Feature *const *f = FEATURES; *f; f++)
        if (strcmp((*f)->URI, uri) == 0) return 1;
    return 0;
}

/* ---- 3. State ---- */

/* Copied, not pointed at: lilv's nodes belong to the world the next scan
 * frees. */
typedef struct {
    char uri[512];
    char name[256];
} desc_t;

typedef struct {
    LilvWorld        *world;
    const LilvPlugins *plugins;      /* owned by the world */
    const LilvPlugin *plugin;        /* owned by the world */
    LilvInstance     *inst;

    int    open;
    double sample_rate;
    long   block, chan;

    /* Descriptor cache from the last scan; allocated at scan time, never
     * in the processing path. */
    long    n_desc;
    long    cap_desc;
    desc_t *desc;
    char   plugin_name[256];
    char   plugin_uri[512];
    char   lv2_path[2048];           /* the path the world was loaded with */

    long   n_ports;
    long   n_audio_in, n_audio_out;
    long   latency_port;             /* -1 when the plugin reports none */

    long   n_params;                    /* parameters = control input ports */
    long   p_port[LV2_HOST_MAX_PARAMS];
    double p_min[LV2_HOST_MAX_PARAMS], p_max[LV2_HOST_MAX_PARAMS], p_def[LV2_HOST_MAX_PARAMS];
    char   p_name[LV2_HOST_MAX_PARAMS][128];
    char   p_sym[LV2_HOST_MAX_PARAMS][128];

    /* One float per port: what a control port is connected to. Audio ports
     * are connected to the block buffers below, so their entry is unused. */
    float  ctrl[LV2_HOST_MAX_PORTS];
    signed char is_ctrl_in[LV2_HOST_MAX_PORTS];
    signed char is_ctrl_out[LV2_HOST_MAX_PORTS];

    /* Each atom:AtomPort gets one buffer carved out of `atom_arena`, a
     * single allocation made at open and freed at close. Slots are
     * numbered in port order; `atom_slot` maps a port to its slot. */
    long    n_atom;
    long    atom_port[LV2_HOST_MAX_ATOM_PORTS];    /* port index of slot i */
    long    atom_slot[LV2_HOST_MAX_PORTS];         /* port index -> slot */
    size_t  atom_off[LV2_HOST_MAX_ATOM_PORTS];     /* byte offset in arena */
    size_t  atom_cap[LV2_HOST_MAX_ATOM_PORTS];     /* buffer bytes per slot */
    long    atom_nev[LV2_HOST_MAX_ATOM_PORTS];     /* events queued (input) */
    int64_t atom_last[LV2_HOST_MAX_ATOM_PORTS];    /* last queued frame */
    signed char atom_in[LV2_HOST_MAX_ATOM_PORTS];  /* 1 input, 0 output */
    signed char atom_midi[LV2_HOST_MAX_ATOM_PORTS];/* 1 declared midi:MidiEvent */
    uint8_t *atom_arena;
    LV2_URID u_sequence, u_frametime, u_midievent, u_chunk;

    float  in[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  out[LV2_HOST_MAX_CHAN][LV2_HOST_MAX_BLOCK];
    float  sink[LV2_HOST_MAX_BLOCK];   /* outputs beyond `chan` go here */
    float  side_in[LV2_HOST_MAX_BLOCK];/* non-main audio inputs read silence */

    long   in_token, out_token;
    int    in_armed;    /* 1 between fill/tone and the process that consumes it */
    long   in_n, out_n;
    long   n_process;
    int    midi_bad;    /* a refused MIDI event poisons the armed block */
} state_t;

static state_t S;

/* ---- 4. Discovery ---- */

static void free_world(void) {
    if (S.inst) {
        if (S.open) lilv_instance_deactivate(S.inst);
        lilv_instance_free(S.inst);
        S.inst = NULL;
    }
    free(S.atom_arena);
    S.atom_arena = NULL;
    S.plugin = NULL;
    S.plugins = NULL;
    if (S.world) lilv_world_free(S.world);
    S.world = NULL;
}

static const char *node_str(const LilvNode *n) {
    return n ? lilv_node_as_string(n) : "";
}

/* The point past which an enumeration is a runaway (the cache itself grows
 * to fit). */
#define LV2_HOST_SCAN_SANITY 65536

static int grow_desc(long n) {
    if (n <= S.cap_desc) return 1;
    long cap = S.cap_desc ? S.cap_desc : 64;
    while (cap < n) cap *= 2;
    desc_t *p = (desc_t *)realloc(S.desc, (size_t)cap * sizeof(desc_t));
    if (!p) return 0;
    S.desc = p;
    S.cap_desc = cap;
    return 1;
}

long lv2_host_scan(const char *lv2_path) {
    ERR[0] = '\0';
    lv2_host_close();
    S.n_desc = 0;   /* a failed scan leaves 0, not the previous path's count */

    S.world = lilv_world_new();
    if (!S.world) { set_err("lilv_world_new failed"); return -1; }

    if (lv2_path && lv2_path[0]) {
        LilvNode *p = lilv_new_string(S.world, lv2_path);
        lilv_world_set_option(S.world, LILV_OPTION_LV2_PATH, p);
        lilv_node_free(p);
        snprintf(S.lv2_path, sizeof S.lv2_path, "%s", lv2_path);
    } else {
        S.lv2_path[0] = '\0';
    }
    const char *where = S.lv2_path[0] ? S.lv2_path : "(default LV2_PATH)";
    lilv_world_load_all(S.world);
    S.plugins = lilv_world_get_all_plugins(S.world);

    LILV_FOREACH (plugins, i, S.plugins) {
        if (S.n_desc >= LV2_HOST_SCAN_SANITY) {
            set_err("more than %d LV2 plugins under '%s'; refusing to enumerate further",
                    LV2_HOST_SCAN_SANITY, where);
            free_world();
            S.n_desc = 0;
            return -1;
        }
        if (!grow_desc(S.n_desc + 1)) {
            set_err("out of memory caching %ld descriptors from '%s'", S.n_desc + 1, where);
            free_world();
            S.n_desc = 0;
            return -1;
        }
        const LilvPlugin *p = lilv_plugins_get(S.plugins, i);
        snprintf(S.desc[S.n_desc].uri, sizeof S.desc[0].uri, "%s",
                 node_str(lilv_plugin_get_uri(p)));
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(S.desc[S.n_desc].name, sizeof S.desc[0].name, "%s", node_str(name));
        lilv_node_free(name);
        S.n_desc++;
    }
    if (S.n_desc == 0) {
        set_err("no LV2 plugins found under '%s'", where);
        free_world();
        return -1;
    }
    return S.n_desc;
}

const char *lv2_host_scan_uri(long i)  { return (i >= 0 && i < S.n_desc) ? S.desc[i].uri  : ""; }
const char *lv2_host_scan_name(long i) { return (i >= 0 && i < S.n_desc) ? S.desc[i].name : ""; }

/* ---- 5. Open / close ---- */

static int port_is(const LilvPlugin *p, const LilvPort *port, const char *cls) {
    LilvNode *n = lilv_new_uri(S.world, cls);
    int r = lilv_port_is_a(p, port, n);
    lilv_node_free(n);
    return r;
}

static int port_has(const LilvPlugin *p, const LilvPort *port, const char *prop) {
    LilvNode *n = lilv_new_uri(S.world, prop);
    int r = lilv_port_has_property(p, port, n);
    lilv_node_free(n);
    return r;
}

/* Connectable when atom:Sequence is among the port's atom:bufferType
 * values (or it declares none, Sequence being the default). `other`
 * points at a static copy of the first non-Sequence value, for the error
 * message when none fit. */
static void atom_buffer_type(const LilvPlugin *p, const LilvPort *port,
                             int *seq_ok, const char **other) {
    static char other_uri[512];
    LilvNode *pred = lilv_new_uri(S.world, LV2_ATOM__bufferType);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    int saw_seq = 0, any = 0;
    *other = NULL;
    LILV_FOREACH (nodes, i, vals) {
        const char *v = node_str(lilv_nodes_get(vals, i));
        any = 1;
        if (strcmp(v, LV2_ATOM__Sequence) == 0) saw_seq = 1;
        else if (!*other) {
            snprintf(other_uri, sizeof other_uri, "%s", v);
            *other = other_uri;
        }
    }
    lilv_nodes_free(vals);
    *seq_ok = saw_seq || !any;
}

/* Declared rsz:minimumSize in bytes, or 0 when undeclared. A double so an
 * overflowing value is refused, not truncated. */
static double atom_min_size(const LilvPlugin *p, const LilvPort *port) {
    LilvNode *pred = lilv_new_uri(S.world, LV2_RESIZE_PORT__minimumSize);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    double r = 0.0;
    if (vals) {
        const LilvNode *v = lilv_nodes_get_first(vals);
        if (v) r = lilv_node_is_int(v) ? (double)lilv_node_as_int(v)
                                      : lilv_node_as_float(v);
        lilv_nodes_free(vals);
    }
    return r;
}

/* 1 when an audio port is on the plugin's main signal path. A port
 * marked lv2:isSideChain, or carrying (or grouped under) pg:sideChainOf,
 * is not main. When the plugin names a pg:mainInput / pg:mainOutput
 * group, a port is main exactly when its pg:group is that group. With
 * no sidechain or port-group information a port is main. */
static int port_main(const LilvPlugin *p, const LilvPort *port, int is_in) {
    if (port_has(p, port, LV2_CORE__isSideChain)) return 0;
    LilvNode *pred = lilv_new_uri(S.world, LV2_PG__sideChainOf);
    LilvNodes *sc = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    if (sc) { lilv_nodes_free(sc); return 0; }

    pred = lilv_new_uri(S.world, is_in ? LV2_PG__mainInput : LV2_PG__mainOutput);
    LilvNodes *mains =
        lilv_world_get(S.world, lilv_plugin_get_uri(p), pred, NULL);
    lilv_node_free(pred);
    if (!mains) return 1;

    int main = 0;
    pred = lilv_new_uri(S.world, LV2_PG__group);
    LilvNodes *groups = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    LILV_FOREACH (nodes, i, groups) {
        const LilvNode *g = lilv_nodes_get(groups, i);
        pred = lilv_new_uri(S.world, LV2_PG__sideChainOf);
        LilvNodes *gsc = lilv_world_get(S.world, g, pred, NULL);
        lilv_node_free(pred);
        int gside = gsc != NULL;
        lilv_nodes_free(gsc);
        if (gside) continue;
        LILV_FOREACH (nodes, j, mains)
            if (lilv_node_equals(lilv_nodes_get(mains, j), g)) main = 1;
    }
    lilv_nodes_free(groups);
    lilv_nodes_free(mains);
    return main;
}

/* MIDI only when atom:supports names midi:MidiEvent, as jalv does. */
static int atom_midi_ok(const LilvPlugin *p, const LilvPort *port) {
    LilvNode *pred = lilv_new_uri(S.world, LV2_ATOM__supports);
    LilvNodes *vals = lilv_port_get_value(p, port, pred);
    lilv_node_free(pred);
    int midi = 0;
    LILV_FOREACH (nodes, i, vals)
        if (strcmp(node_str(lilv_nodes_get(vals, i)), LV2_MIDI__MidiEvent) == 0)
            midi = 1;
    lilv_nodes_free(vals);
    return midi;
}

static void fail_open(void) { lv2_host_close(); }
static void head_atom_seq(long s);
static void head_atom_out(long s);

int lv2_host_open(const char *lv2_path, const char *uri,
                  double sample_rate, double block_size, double channels) {
    long n = lv2_host_scan(lv2_path);      /* also clears state and sets ERR */
    if (n < 0) return 1;

    if (!isfinite(block_size) || !isfinite(channels)) {
        set_err("block_size and channels must be finite, got %g and %g",
                block_size, channels);
        fail_open();
        return 1;
    }
    long blk = (long)(block_size + 0.5);
    long ch  = (long)(channels + 0.5);
    if (blk < 1 || blk > LV2_HOST_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, LV2_HOST_MAX_BLOCK);
        fail_open();
        return 1;
    }
    if (ch < 1 || ch > LV2_HOST_MAX_CHAN) {
        set_err("channels %ld out of range 1..%d", ch, LV2_HOST_MAX_CHAN);
        fail_open();
        return 1;
    }
    if (!(sample_rate > 0.0)) {
        set_err("sample_rate must be positive, got %g", sample_rate);
        fail_open();
        return 1;
    }

    const char *want = (uri && uri[0]) ? uri : S.desc[0].uri;
    LilvNode *want_node = lilv_new_uri(S.world, want);
    const LilvPlugin *p = want_node ? lilv_plugins_get_by_uri(S.plugins, want_node) : NULL;
    lilv_node_free(want_node);
    if (!p) {
        set_err("no plugin with URI '%s' under '%s' (found %ld: first is '%s')",
                want, S.lv2_path[0] ? S.lv2_path : "(default LV2_PATH)",
                S.n_desc, S.desc[0].uri);
        fail_open();
        return 1;
    }
    S.plugin = p;
    snprintf(S.plugin_uri, sizeof S.plugin_uri, "%s", node_str(lilv_plugin_get_uri(p)));
    {
        LilvNode *name = lilv_plugin_get_name(p);
        snprintf(S.plugin_name, sizeof S.plugin_name, "%s", node_str(name));
        lilv_node_free(name);
    }

    LilvNodes *req = lilv_plugin_get_required_features(p);
    LILV_FOREACH (nodes, i, req) {
        const char *f = node_str(lilv_nodes_get(req, i));
        if (!feature_supported(f)) {
            set_err("plugin '%s' requires host feature <%s>, which this host does not provide",
                    S.plugin_uri, f);
            lilv_nodes_free(req);
            fail_open();
            return 1;
        }
    }
    lilv_nodes_free(req);

    S.n_ports = (long)lilv_plugin_get_num_ports(p);
    if (S.n_ports > LV2_HOST_MAX_PORTS) {
        set_err("plugin '%s' has %ld ports, more than this host's %d",
                S.plugin_uri, S.n_ports, LV2_HOST_MAX_PORTS);
        fail_open();
        return 1;
    }
    S.n_audio_in = S.n_audio_out = 0;
    S.n_params = 0;
    S.latency_port = -1;
    memset(S.is_ctrl_in, 0, sizeof S.is_ctrl_in);
    memset(S.is_ctrl_out, 0, sizeof S.is_ctrl_out);
    S.n_atom = 0;
    memset(S.atom_slot, -1, sizeof S.atom_slot);

    float *mins = (float *)calloc((size_t)S.n_ports, sizeof(float));
    float *maxs = (float *)calloc((size_t)S.n_ports, sizeof(float));
    float *defs = (float *)calloc((size_t)S.n_ports, sizeof(float));
    if (!mins || !maxs || !defs) {
        free(mins); free(maxs); free(defs);
        set_err("out of memory");
        fail_open();
        return 1;
    }
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    typedef enum {
        K_AUDIO_IN, K_AUDIO_OUT, K_AUDIO_IN_SIDE, K_AUDIO_OUT_SIDE,
        K_CTRL_IN, K_CTRL_OUT, K_ATOM_IN, K_ATOM_OUT, K_NONE
    } kind_t;
    kind_t kind[LV2_HOST_MAX_PORTS];
    size_t arena_bytes = 0;

    for (long k = 0; k < S.n_ports; k++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, (uint32_t)k);
        const char *sym = node_str(lilv_port_get_symbol(p, port));
        int is_in  = port_is(p, port, LILV_URI_INPUT_PORT);
        int is_out = port_is(p, port, LILV_URI_OUTPUT_PORT);
        int audio  = port_is(p, port, LILV_URI_AUDIO_PORT);
        int ctrl   = port_is(p, port, LILV_URI_CONTROL_PORT);
        int atom   = port_is(p, port, LILV_URI_ATOM_PORT);

        if (audio && is_in) {
            if (port_main(p, port, 1)) { kind[k] = K_AUDIO_IN;      S.n_audio_in++; }
            else                       { kind[k] = K_AUDIO_IN_SIDE; }
        }
        else if (audio && is_out) {
            if (port_main(p, port, 0)) { kind[k] = K_AUDIO_OUT;      S.n_audio_out++; }
            else                       { kind[k] = K_AUDIO_OUT_SIDE; }
        }
        else if (ctrl && is_in) {
            kind[k] = K_CTRL_IN;
            S.is_ctrl_in[k] = 1;
            /* An untouched control port is its declared default. */
            S.ctrl[k] = isnan(defs[k]) ? 0.0f : defs[k];
            if (S.n_params < LV2_HOST_MAX_PARAMS) {
                long j = S.n_params++;
                S.p_port[j] = k;
                S.p_min[j]  = mins[k];
                S.p_max[j]  = maxs[k];
                S.p_def[j]  = defs[k];
                LilvNode *nm = lilv_port_get_name(p, port);
                snprintf(S.p_name[j], sizeof S.p_name[0], "%s", node_str(nm));
                lilv_node_free(nm);
                snprintf(S.p_sym[j], sizeof S.p_sym[0], "%s", sym);
            }
        }
        else if (ctrl && is_out) { kind[k] = K_CTRL_OUT; S.is_ctrl_out[k] = 1; S.ctrl[k] = 0.0f; }
        else if (atom && (is_in || is_out)) {
            /* A declared bufferType other than atom:Sequence is only an
             * error when the port is required -- then it is named. */
            int seq_ok; const char *other;
            atom_buffer_type(p, port, &seq_ok, &other);
            if (!seq_ok) {
                if (port_has(p, port, LV2_CORE__connectionOptional)) { kind[k] = K_NONE; }
                else {
                    set_err("plugin '%s' port %ld ('%s') is a required atom port with "
                            "bufferType <%s>, which this host cannot connect "
                            "(only atom:Sequence is supported)",
                            S.plugin_uri, k, sym, other);
                    free(mins); free(maxs); free(defs);
                    fail_open();
                    return 1;
                }
            } else if (S.n_atom >= LV2_HOST_MAX_ATOM_PORTS) {
                set_err("plugin '%s' has more than %d atom ports, which this host "
                        "cannot connect", S.plugin_uri, LV2_HOST_MAX_ATOM_PORTS);
                free(mins); free(maxs); free(defs);
                fail_open();
                return 1;
            } else {
                double msz = atom_min_size(p, port);
                if (msz > (double)UINT32_MAX) {
                    set_err("plugin '%s' port %ld ('%s') declares "
                            "rsz:minimumSize %.0f, more than %lu bytes",
                            S.plugin_uri, k, sym, msz, (unsigned long)UINT32_MAX);
                    free(mins); free(maxs); free(defs);
                    fail_open();
                    return 1;
                }
                long s = S.n_atom++;
                kind[k] = is_in ? K_ATOM_IN : K_ATOM_OUT;
                S.atom_slot[k] = s;
                S.atom_port[s] = k;
                S.atom_in[s]   = is_in;
                S.atom_midi[s] = (signed char)atom_midi_ok(p, port);
                S.atom_cap[s]  = (size_t)(msz > LV2_HOST_ATOM_BUF ? msz : LV2_HOST_ATOM_BUF);
                S.atom_cap[s]  = (S.atom_cap[s] + 7u) & ~(size_t)7u;
                S.atom_off[s]  = arena_bytes;
                arena_bytes   += S.atom_cap[s];
                if (arena_bytes > LV2_HOST_ATOM_ARENA_MAX) {
                    set_err("plugin '%s' asks for more than %u bytes of atom "
                            "buffers, which this host will not allocate",
                            S.plugin_uri, (unsigned)LV2_HOST_ATOM_ARENA_MAX);
                    free(mins); free(maxs); free(defs);
                    fail_open();
                    return 1;
                }
                S.atom_nev[s]  = 0;
                S.atom_last[s] = -1;
            }
        }
        else if (port_has(p, port, LV2_CORE__connectionOptional)) { kind[k] = K_NONE; }
        else {
            /* CV, event, or something newer: a required port of a class
             * this host cannot feed. Say which. */
            const char *cls = port_is(p, port, LILV_URI_CV_PORT)     ? "CV"    :
                              port_is(p, port, LILV_URI_EVENT_PORT)  ? "event" :
                              port_is(p, port, LILV_URI_ATOM_PORT)   ? "atom"  : "unknown-class";
            set_err("plugin '%s' port %ld ('%s') is a required %s port, which this host "
                    "cannot connect", S.plugin_uri, k, sym, cls);
            free(mins); free(maxs); free(defs);
            fail_open();
            return 1;
        }
    }
    free(mins); free(maxs); free(defs);
    /* More host channels than main audio inputs would silently drop a
     * channel. */
    if (S.n_audio_in > 0 && S.n_audio_in < ch) {
        set_err("plugin '%s' has %ld main audio input port%s but the host was asked "
                "for %ld channels; host channel %ld would feed no port",
                S.plugin_uri, S.n_audio_in, S.n_audio_in == 1 ? "" : "s",
                ch, S.n_audio_in);
        fail_open();
        return 1;
    }
    if (lilv_plugin_has_latency(p))
        S.latency_port = (long)lilv_plugin_get_latency_port_index(p);

    if (S.n_atom > 0) {
        S.atom_arena = (uint8_t *)malloc(arena_bytes);
        if (!S.atom_arena) {
            set_err("out of memory allocating %zu bytes of atom buffers for '%s'",
                    arena_bytes, S.plugin_uri);
            fail_open();
            return 1;
        }
        /* The atom URIDs the host itself stamps on buffers and events;
         * 0 means the map table is full. */
        S.u_sequence  = urid_map(NULL, LV2_ATOM__Sequence);
        S.u_frametime = urid_map(NULL, LV2_ATOM__frameTime);
        S.u_midievent = urid_map(NULL, LV2_MIDI__MidiEvent);
        S.u_chunk     = urid_map(NULL, LV2_ATOM__Chunk);
        if (!S.u_sequence || !S.u_frametime || !S.u_midievent || !S.u_chunk) {
            set_err("URID map table full while mapping atom types for '%s'", S.plugin_uri);
            fail_open();
            return 1;
        }
    }

    S.inst = lilv_plugin_instantiate(p, sample_rate, FEATURES);
    if (!S.inst) {
        set_err("plugin '%s' refused to instantiate at %g Hz", S.plugin_uri, sample_rate);
        fail_open();
        return 1;
    }

    /* Audio: k-th main input port <- host channel min(k, ch-1); k-th
     * main output port -> host channel k, or the sink when k >= ch.
     * Non-main inputs read a zeroed buffer, non-main outputs the sink. */
    memset(S.side_in, 0, sizeof S.side_in);
    long ai = 0, ao = 0;
    for (long k = 0; k < S.n_ports; k++) {
        switch (kind[k]) {
        case K_AUDIO_IN:
            lilv_instance_connect_port(S.inst, (uint32_t)k, S.in[ai < ch ? ai : ch - 1]);
            ai++;
            break;
        case K_AUDIO_IN_SIDE:
            lilv_instance_connect_port(S.inst, (uint32_t)k, S.side_in);
            break;
        case K_AUDIO_OUT:
            lilv_instance_connect_port(S.inst, (uint32_t)k, ao < ch ? S.out[ao] : S.sink);
            ao++;
            break;
        case K_AUDIO_OUT_SIDE:
            lilv_instance_connect_port(S.inst, (uint32_t)k, S.sink);
            break;
        case K_CTRL_IN:
        case K_CTRL_OUT:
            lilv_instance_connect_port(S.inst, (uint32_t)k, &S.ctrl[k]);
            break;
        case K_ATOM_IN:
        case K_ATOM_OUT: {
            long s = S.atom_slot[k];
            lilv_instance_connect_port(S.inst, (uint32_t)k,
                                       S.atom_arena + S.atom_off[s]);
            if (S.atom_in[s]) head_atom_seq(s);
            else              head_atom_out(s);
            break;
        }
        case K_NONE:
            lilv_instance_connect_port(S.inst, (uint32_t)k, NULL);
            break;
        }
    }

    lilv_instance_activate(S.inst);

    S.sample_rate = sample_rate;
    S.block = blk;
    S.chan  = ch;
    memset(S.in, 0, sizeof S.in);
    memset(S.out, 0, sizeof S.out);
    S.in_token = S.out_token = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.open = 1;
    return 0;
}

void lv2_host_close(void) {
    free_world();
    for (uint32_t i = 0; i < N_URIS; i++) { free(URIS[i]); URIS[i] = NULL; }
    N_URIS = 0;
    S.open = 0;
    S.n_params = 0;
    S.n_ports = 0;
    S.n_audio_in = S.n_audio_out = 0;
    S.latency_port = -1;
    S.n_atom = 0;
    memset(S.atom_slot, -1, sizeof S.atom_slot);
    S.u_sequence = S.u_frametime = S.u_midievent = S.u_chunk = 0;
    S.in_token = S.out_token = 0;
    S.in_armed = 0;
    S.in_n = S.out_n = 0;
    S.n_process = 0;
    S.midi_bad = 0;
    S.plugin_name[0] = '\0';
    S.plugin_uri[0] = '\0';
    /* The descriptor cache survives a failed open, as in clap_host.c. */
}

const char *lv2_host_plugin_name(void) { return S.plugin_name; }
const char *lv2_host_plugin_uri(void)  { return S.plugin_uri; }

/* ---- 6. Parameter and configuration reporting ---- */

long lv2_host_n_params(void) { return S.n_params; }

static int pidx(long i) { return (i >= 0 && i < S.n_params); }

double lv2_host_param_id(long i)        { return pidx(i) ? (double)S.p_port[i] : -1.0; }
double lv2_host_param_min(long i)       { return pidx(i) ? S.p_min[i] : NAN; }
double lv2_host_param_max(long i)       { return pidx(i) ? S.p_max[i] : NAN; }
double lv2_host_param_default(long i)   { return pidx(i) ? S.p_def[i] : NAN; }
const char *lv2_host_param_name(long i)   { return pidx(i) ? S.p_name[i] : ""; }
const char *lv2_host_param_symbol(long i) { return pidx(i) ? S.p_sym[i] : ""; }

double lv2_host_param_value(double port_index) {
    if (!S.open || !isfinite(port_index)) return NAN;
    long k = (long)(port_index + 0.5);
    if (k < 0 || k >= S.n_ports || !S.is_ctrl_in[k]) return NAN;
    return (double)S.ctrl[k];
}

double lv2_host_port_value(double port_index) {
    if (!S.open || !isfinite(port_index)) return NAN;
    long k = (long)(port_index + 0.5);
    if (k < 0 || k >= S.n_ports || (!S.is_ctrl_in[k] && !S.is_ctrl_out[k])) return NAN;
    return (double)S.ctrl[k];
}

static int aidx(long i) { return (i >= 0 && i < S.n_atom); }

double lv2_host_n_atom_ports(void) { return S.open ? (double)S.n_atom : 0.0; }

static int aslot(double i, long *s) {
    if (!S.open || !isfinite(i)) return 0;
    *s = (long)(i + 0.5);
    return aidx(*s);
}

double lv2_host_atom_port_index(double i) {
    long s;
    return aslot(i, &s) ? (double)S.atom_port[s] : NAN;
}

double lv2_host_atom_port_is_input(double i) {
    long s;
    return aslot(i, &s) ? (double)S.atom_in[s] : NAN;
}

double lv2_host_atom_port_midi(double i) {
    long s;
    return aslot(i, &s) ? (double)S.atom_midi[s] : NAN;
}

double lv2_host_atom_port_size(double i) {
    long s;
    return aslot(i, &s) ? (double)S.atom_cap[s] : NAN;
}

double lv2_host_latency(void) {
    if (!S.open || S.latency_port < 0) return 0.0;
    return (double)S.ctrl[S.latency_port];
}

double lv2_host_n_audio_in(void)  { return S.open ? (double)S.n_audio_in  : 0.0; }
double lv2_host_n_audio_out(void) { return S.open ? (double)S.n_audio_out : 0.0; }
double lv2_host_sample_rate(void) { return S.open ? S.sample_rate : 0.0; }
double lv2_host_block_size(void)  { return S.open ? (double)S.block : 0.0; }
double lv2_host_channels(void)    { return S.open ? (double)S.chan : 0.0; }
double lv2_host_is_open(void)     { return S.open ? 1.0 : 0.0; }
long   lv2_host_n_process(void)   { return S.n_process; }
void   lv2_host_reset_counters(void) { S.n_process = 0; }

/* ---- 7. The input block ---- */

/* Inputs are headed with an empty atom:Sequence in frame time; outputs
 * with an atom:Chunk whose size is the space after the atom header --
 * what atom.meta.ttl requires of a variable-size output. */
static void head_atom_seq(long s) {
    LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)(S.atom_arena + S.atom_off[s]);
    seq->atom.type = S.u_sequence;
    seq->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    seq->body.unit = S.u_frametime;
    seq->body.pad  = 0;
}

static void head_atom_out(long s) {
    LV2_Atom *a = (LV2_Atom *)(S.atom_arena + S.atom_off[s]);
    a->type = S.u_chunk;
    a->size = (uint32_t)(S.atom_cap[s] - sizeof(LV2_Atom));
}

/* A new input block carries no events and clears a refused event. */
static void rehead_atom_inputs(void) {
    for (long s = 0; s < S.n_atom; s++) {
        if (!S.atom_in[s]) continue;
        head_atom_seq(s);
        S.atom_nev[s]  = 0;
        S.atom_last[s] = -1;
    }
    S.midi_bad = 0;
}

double lv2_in_fill(const double *samples, long n, long channels) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!samples || n < 0) { set_err("lv2_in_fill: no samples"); return NAN; }
    if (n > S.block) n = S.block;
    long ch = (channels < 1) ? 1 : (channels > S.chan ? S.chan : channels);
    for (long i = 0; i < n; i++)
        for (long c = 0; c < S.chan; c++) {
            long src = (c < ch) ? (i * ch + c) : (i * ch);   /* mono -> all */
            S.in[c][i] = (float)samples[src];
        }
    for (long i = n; i < S.block; i++)
        for (long c = 0; c < S.chan; c++) S.in[c][i] = 0.0f;
    rehead_atom_inputs();
    S.in_n = n;
    S.in_armed = 1;
    return (double)(++S.in_token);
}

static double wave_at(long k, int w, double freq, double amp) {
    double t = (double)k / S.sample_rate;
    switch (w) {
    case LV2_WAVE_SINE:    return amp * sin(2.0 * M_PI * freq * t);
    case LV2_WAVE_SQUARE:  return amp * (sin(2.0 * M_PI * freq * t) >= 0.0 ? 1.0 : -1.0);
    case LV2_WAVE_RAMP:    return amp * (2.0 * fmod(freq * t, 1.0) - 1.0);
    case LV2_WAVE_IMPULSE: return (k == 0) ? amp : 0.0;
    default:               return 0.0;
    }
}

double lv2_in_tone(double t, double waveform, double freq, double amp) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    double end = t * S.sample_rate;
    if (!isfinite(end) || !isfinite(waveform)) {
        set_err("lv2_in_tone: time and waveform must be finite");
        return NAN;
    }
    long first = (long)(end + 0.5) - S.block;
    int w = (int)(waveform + 0.5);
    for (long i = 0; i < S.block; i++) {
        double v = wave_at(first + i, w, freq, amp);
        for (long c = 0; c < S.chan; c++) S.in[c][i] = (float)v;
    }
    rehead_atom_inputs();
    S.in_n = S.block;
    S.in_armed = 1;
    return (double)(++S.in_token);
}

double lv2_in_sample(double dep, double i, double ch) {
    if (!S.open || !isfinite(dep) || !isfinite(i) || !isfinite(ch)) return NAN;
    if (!S.in_armed || (long)(dep + 0.5) != S.in_token) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.in_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.in[c][k];
}

/* Bytes a complete message with this status carries: channel messages 3
 * (2 for 0xC0-0xDF program change / channel pressure), the listed system
 * messages, -1 for variable-length sysex this call cannot carry, 0 for
 * undefined statuses. */
static int midi_msg_len(uint8_t st) {
    if (st < 0xC0)  return 3;
    if (st < 0xE0)  return 2;
    if (st < 0xF0)  return 3;
    if (st >= 0xF8) return 1;
    switch (st) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: return 1;
    default:  return (st == 0xF0 || st == 0xF7) ? -1 : 0;
    }
}

/* dep is the input token, returned so a chain orders before lv2_process.
 * A refusal after the token check poisons the armed block. */
static double midi_refused(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
    S.midi_bad = 1;
    return NAN;
}

double lv2_in_midi(double dep, double port, double frame,
                   double b0, double b1, double b2) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!isfinite(dep) || !S.in_armed || (long)(dep + 0.5) != S.in_token) {
        set_err("lv2_in_midi: no input block armed for this token; call "
                "lv2_in_fill or lv2_in_tone first");
        return NAN;
    }
    if (!isfinite(port)) return midi_refused("lv2_in_midi: port is not a number");
    long k = (long)(port + 0.5);
    if (k < 0 || k >= S.n_ports || S.atom_slot[k] < 0)
        return midi_refused("lv2_in_midi: port %ld is not an atom port", k);
    long s = S.atom_slot[k];
    if (!S.atom_in[s])
        return midi_refused("lv2_in_midi: port %ld is an atom output, not an input", k);
    if (!S.atom_midi[s])
        return midi_refused("lv2_in_midi: port %ld does not declare "
                            "atom:supports midi:MidiEvent", k);
    if (!isfinite(frame))
        return midi_refused("lv2_in_midi: frame is not a number");
    long f = (long)(frame + 0.5);
    if (f < 0 || f >= S.block)
        return midi_refused("lv2_in_midi: frame %ld is outside the block (0..%ld)",
                            f, S.block - 1);
    if (f < S.atom_last[s])
        return midi_refused("lv2_in_midi: event at frame %ld is out of order "
                            "after frame %lld (events must be queued in "
                            "non-decreasing frame order)", f, (long long)S.atom_last[s]);

    const double bytes[3] = { b0, b1, b2 };
    uint8_t msg[3];
    int nb = 0, ended = 0;
    for (int i = 0; i < 3; i++) {
        double x = bytes[i];
        if (!isfinite(x) || x < 0) { ended = 1; continue; }
        if (ended)
            return midi_refused("lv2_in_midi: byte %d follows a gap in the "
                                "message", i);
        long b = (long)(x + 0.5);
        if (b > 0xFF)
            return midi_refused("lv2_in_midi: byte %d is %ld, not a MIDI byte",
                                i, b);
        if (i == 0 && b < 0x80)
            return midi_refused("lv2_in_midi: byte 0 is %ld, not a status byte "
                                "(0x80..0xFF)", b);
        if (i > 0 && b > 0x7F)
            return midi_refused("lv2_in_midi: byte %d is %ld, not a data byte "
                                "(0x00..0x7F)", i, b);
        msg[nb++] = (uint8_t)b;
    }
    if (nb == 0)
        return midi_refused("lv2_in_midi: a MIDI event needs a status byte");
    int want = midi_msg_len(msg[0]);
    if (want < 0)
        return midi_refused("lv2_in_midi: status 0x%02lX is variable-length; "
                            "this call carries complete 1-3 byte messages only",
                            (unsigned long)msg[0]);
    if (want == 0)
        return midi_refused("lv2_in_midi: status 0x%02lX is undefined",
                            (unsigned long)msg[0]);
    if (nb != want)
        return midi_refused("lv2_in_midi: status 0x%02lX is a %d-byte message, "
                            "got %d", (unsigned long)msg[0], want, nb);

    LV2_Atom_Sequence *seq =
        (LV2_Atom_Sequence *)(S.atom_arena + S.atom_off[s]);
    /* atom.size counts the bytes after the atom header, so capacity is
     * the buffer less that header. */
    const uint32_t cap   = (uint32_t)(S.atom_cap[s] - sizeof(LV2_Atom));
    const uint32_t total = (uint32_t)sizeof(LV2_Atom_Event) + (uint32_t)nb;
    if (cap - seq->atom.size < total)
        return midi_refused("lv2_in_midi: atom input sequence on port %ld is "
                            "full (%u bytes capacity)", k, cap);
    LV2_Atom_Event *e = lv2_atom_sequence_end(&seq->body, seq->atom.size);
    e->time.frames = (int64_t)f;
    e->body.size   = (uint32_t)nb;
    e->body.type   = S.u_midievent;
    memcpy(LV2_ATOM_BODY(&e->body), msg, (size_t)nb);
    seq->atom.size += lv2_atom_pad_size(total);
    S.atom_last[s] = f;
    S.atom_nev[s]++;
    return dep;
}

/* ---- 8. run() -- the node-side operator ---- */

double lv2_process(double dep,
                   double id0, double v0, double id1, double v1,
                   double id2, double v2, double id3, double v3) {
    if (!S.open) { set_err("no plugin is open"); return NAN; }
    if (!isfinite(dep) || !S.in_armed || (long)(dep + 0.5) != S.in_token)
        return NAN;
    if (S.midi_bad) {
        set_err("lv2_process: the armed block carries a refused MIDI event; "
                "fill a fresh input block");
        return NAN;
    }
    /* One block, one run: a second lv2_process on this dep refuses
     * instead of replaying queued events. */
    S.in_armed = 0;

    const double ids[LV2_HOST_PARAM_SLOTS]  = { id0, id1, id2, id3 };
    const double vals[LV2_HOST_PARAM_SLOTS] = { v0,  v1,  v2,  v3  };
    for (int i = 0; i < LV2_HOST_PARAM_SLOTS; i++) {
        if (!isfinite(ids[i]) || !(ids[i] >= 0.0) || isnan(vals[i])) continue;
        long k = (long)(ids[i] + 0.5);
        if (k < S.n_ports && S.is_ctrl_in[k]) S.ctrl[k] = (float)vals[i];
    }

    for (long s = 0; s < S.n_atom; s++)
        if (!S.atom_in[s]) head_atom_out(s);
    memset(S.side_in, 0, sizeof S.side_in);

    lilv_instance_run(S.inst, (uint32_t)S.block);
    S.n_process++;

    /* Fewer outputs than channels: repeat the last output (a mono plugin
     * is centre-panned). Zero outputs: the channels keep open()'s silence. */
    for (long c = S.n_audio_out; c < S.chan && S.n_audio_out > 0; c++)
        memcpy(S.out[c], S.out[S.n_audio_out - 1],
               (size_t)S.block * sizeof(float));

    S.out_n = S.block;
    return (double)(++S.out_token);
}

/* ---- 9. The output block ---- */

static int out_ok(double dep) {
    return S.open && isfinite(dep) && S.out_token != 0 &&
           (long)(dep + 0.5) == S.out_token;
}

double lv2_out_sample(double dep, double i, double ch) {
    if (!out_ok(dep) || !isfinite(i) || !isfinite(ch)) return NAN;
    long k = (long)(i + 0.5), c = (long)(ch + 0.5);
    if (k < 0 || k >= S.out_n || c < 0 || c >= S.chan) return NAN;
    return (double)S.out[c][k];
}

double lv2_out_rms(double dep) {
    if (!out_ok(dep)) return NAN;
    if (S.out_n <= 0) return 0.0;
    double s = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) s += (double)S.out[c][i] * (double)S.out[c][i];
    return sqrt(s / (double)(S.out_n * S.chan));
}

double lv2_out_peak(double dep) {
    if (!out_ok(dep)) return NAN;
    double m = 0.0;
    for (long c = 0; c < S.chan; c++)
        for (long i = 0; i < S.out_n; i++) {
            double a = fabs((double)S.out[c][i]);
            if (a > m) m = a;
        }
    return m;
}

double lv2_out_count(double dep) { return out_ok(dep) ? (double)S.out_n : NAN; }
double lv2_out_valid(double dep) { return out_ok(dep) ? 1.0 : 0.0; }
