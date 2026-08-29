/* lv2_host.c -- hosting an LV2 plugin, audio path only.
 *
 * Same shape as clap_host.c and the same reasons: fixed C symbol names,
 * scalar doubles across the node-side boundary, a dep-threaded token
 * naming a block, loud failures.
 *
 * WHAT THIS DOES AND DOES NOT DO
 *
 * The LV2 *audio* path is smaller than CLAP's: one vendored header
 * (vendor/lv2/core/lv2.h, 472 lines, ISC), dlopen, resolve
 * lv2_descriptor(), then instantiate / connect_port / activate / run /
 * deactivate / cleanup. Buffers are connected once by port INDEX and the
 * plugin reads them on every run(), so a fixed block size is not merely
 * supported, it is the natural shape.
 *
 * DISCOVERY IS NOT IMPLEMENTED, deliberately. An LV2 plugin's port
 * layout -- which index is audio-in, which is audio-out, which is a
 * control, and what a control's range is -- lives in Turtle/RDF manifests
 * next to the binary, not in the binary. Reading those means either
 * depending on lilv (which needs serd, sord and sratom, and none of the
 * four are JLLs today) or writing an RDF parser, and a half-correct RDF
 * parser silently mis-mapping a port is worse than no discovery at all.
 *
 * So this host makes the port map an explicit argument: the caller says
 * which index is in, which is out, and which is control. That is honest,
 * it is enough to host a plugin whose manifest you have read, and it
 * keeps the fragile part out of the package. See the report and the
 * README for what full discovery would cost.
 */

#include "lv2_host.h"
#include "vendor/lv2/core/lv2.h"

#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LV2_MAX_BLOCK 8192
#define LV2_MAX_CTRL  8

static char ERR[512];

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ERR, sizeof ERR, fmt, ap);
    va_end(ap);
}

const char *lv2_host_last_error(void) { return ERR; }

typedef struct {
    void                *dl;
    const LV2_Descriptor *desc;
    LV2_Handle            h;
    int    open;
    long   block;
    long   in_port, out_port;
    long   ctrl_port[LV2_MAX_CTRL];
    long   n_ctrl;
    float  in[LV2_MAX_BLOCK];
    float  out[LV2_MAX_BLOCK];
    float  ctrl[LV2_MAX_CTRL];
    char   uri[512];
    long   in_token, out_token, n_run;
} lstate_t;

static lstate_t L;

void lv2_host_close(void) {
    if (L.h && L.desc) {
        if (L.desc->deactivate) L.desc->deactivate(L.h);
        if (L.desc->cleanup) L.desc->cleanup(L.h);
    }
    L.h = NULL;
    L.desc = NULL;
    if (L.dl) dlclose(L.dl);
    L.dl = NULL;
    L.open = 0;
    L.in_token = L.out_token = L.n_run = 0;
    L.n_ctrl = 0;
    L.uri[0] = '\0';
}

/* Enumerate the descriptors a binary exports. Returns the count (stopping
 * at the first NULL, as the ABI specifies), or -1 on failure. */
long lv2_host_scan(const char *path) {
    ERR[0] = '\0';
    lv2_host_close();
    if (!path || !path[0]) { set_err("no plugin path given"); return -1; }

    L.dl = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (!L.dl) { set_err("dlopen %s: %s", path, dlerror()); return -1; }

    LV2_Descriptor_Function fn =
        (LV2_Descriptor_Function)dlsym(L.dl, "lv2_descriptor");
    if (!fn) {
        set_err("%s has no lv2_descriptor symbol -- not an LV2 plugin", path);
        dlclose(L.dl);
        L.dl = NULL;
        return -1;
    }
    long n = 0;
    while (fn((uint32_t)n) != NULL && n < 1024) n++;
    if (n == 0) {
        set_err("%s exports lv2_descriptor but no descriptors", path);
        dlclose(L.dl);
        L.dl = NULL;
        return -1;
    }
    return n;
}

const char *lv2_host_scan_uri(long i) {
    if (!L.dl) return "";
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(L.dl, "lv2_descriptor");
    if (!fn) return "";
    const LV2_Descriptor *d = fn((uint32_t)i);
    return (d && d->URI) ? d->URI : "";
}

int lv2_host_open(const char *path, const char *uri, double sample_rate,
                  double block_size, double in_port, double out_port,
                  const double *ctrl_ports, long n_ctrl) {
    long n = lv2_host_scan(path);
    if (n < 0) return 1;

    long blk = (long)(block_size + 0.5);
    if (blk < 1 || blk > LV2_MAX_BLOCK) {
        set_err("block_size %ld out of range 1..%d", blk, LV2_MAX_BLOCK);
        lv2_host_close();
        return 1;
    }
    if (n_ctrl < 0 || n_ctrl > LV2_MAX_CTRL) {
        set_err("n_ctrl %ld out of range 0..%d", n_ctrl, LV2_MAX_CTRL);
        lv2_host_close();
        return 1;
    }

    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(L.dl, "lv2_descriptor");
    const LV2_Descriptor *d = NULL;
    for (long i = 0; i < n; i++) {
        const LV2_Descriptor *c = fn((uint32_t)i);
        if (!c) break;
        if (!uri || !uri[0] || (c->URI && strcmp(c->URI, uri) == 0)) { d = c; break; }
    }
    if (!d) {
        set_err("no plugin with URI '%s' in %s (it exports %ld: first is '%s')",
                uri ? uri : "", path, n, lv2_host_scan_uri(0));
        lv2_host_close();
        return 1;
    }
    L.desc = d;
    snprintf(L.uri, sizeof L.uri, "%s", d->URI ? d->URI : "");

    /* An empty, NULL-terminated feature list: this host offers none, and
     * a plugin that requires one is entitled to refuse instantiation. */
    const LV2_Feature *const features[] = { NULL };
    L.h = d->instantiate(d, sample_rate, "", features);
    if (!L.h) {
        set_err("plugin '%s' refused to instantiate at %g Hz "
                "(it may require a host feature this host does not offer)",
                L.uri, sample_rate);
        lv2_host_close();
        return 1;
    }

    L.block = blk;
    L.in_port = (long)(in_port + 0.5);
    L.out_port = (long)(out_port + 0.5);
    L.n_ctrl = n_ctrl;
    for (long i = 0; i < n_ctrl; i++) {
        L.ctrl_port[i] = (long)(ctrl_ports[i] + 0.5);
        L.ctrl[i] = 0.0f;
    }

    /* Buffers are connected once and re-read by every run(). */
    d->connect_port(L.h, (uint32_t)L.in_port, L.in);
    d->connect_port(L.h, (uint32_t)L.out_port, L.out);
    for (long i = 0; i < n_ctrl; i++)
        d->connect_port(L.h, (uint32_t)L.ctrl_port[i], &L.ctrl[i]);

    if (d->activate) d->activate(L.h);
    L.open = 1;
    L.in_token = L.out_token = 0;
    L.n_run = 0;
    return 0;
}

const char *lv2_host_uri(void)      { return L.uri; }
double lv2_host_is_open(void)       { return L.open ? 1.0 : 0.0; }
double lv2_host_block_size(void)    { return L.open ? (double)L.block : 0.0; }
long   lv2_host_n_run(void)         { return L.n_run; }

double lv2_in_fill(const double *samples, long n) {
    if (!L.open) { set_err("no plugin is open"); return NAN; }
    if (!samples) { set_err("lv2_in_fill: no samples"); return NAN; }
    if (n > L.block) n = L.block;
    for (long i = 0; i < n; i++) L.in[i] = (float)samples[i];
    for (long i = n; i < L.block; i++) L.in[i] = 0.0f;
    return (double)(++L.in_token);
}

double lv2_run(double dep, double c0, double c1, double c2, double c3) {
    if (!L.open) { set_err("no plugin is open"); return NAN; }
    if ((long)(dep + 0.5) != L.in_token || L.in_token == 0) return NAN;
    const double cs[4] = { c0, c1, c2, c3 };
    for (long i = 0; i < L.n_ctrl && i < 4; i++)
        if (!isnan(cs[i])) L.ctrl[i] = (float)cs[i];
    L.desc->run(L.h, (uint32_t)L.block);
    L.n_run++;
    return (double)(++L.out_token);
}

static int lout_ok(double dep) {
    return L.open && L.out_token != 0 && (long)(dep + 0.5) == L.out_token;
}

double lv2_out_sample(double dep, double i) {
    if (!lout_ok(dep)) return NAN;
    long k = (long)(i + 0.5);
    if (k < 0 || k >= L.block) return NAN;
    return (double)L.out[k];
}

double lv2_out_rms(double dep) {
    if (!lout_ok(dep)) return NAN;
    double s = 0.0;
    for (long i = 0; i < L.block; i++) s += (double)L.out[i] * (double)L.out[i];
    return sqrt(s / (double)L.block);
}

double lv2_out_peak(double dep) {
    if (!lout_ok(dep)) return NAN;
    double m = 0.0;
    for (long i = 0; i < L.block; i++) {
        double a = fabs((double)L.out[i]);
        if (a > m) m = a;
    }
    return m;
}
