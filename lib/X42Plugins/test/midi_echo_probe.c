/* midi_echo_probe.c -- load an LV2 .so/.dylib/.dll by path, send one note-on,
 * count MIDI events on the atom output. Exit 0 when exactly one matching
 * note-on is forged. Asserts midifilter#passthru actually echoes (LV2Host 1.2
 * does not decode atom outputs).
 *
 * Usage: midi_echo_probe <plugin.so> <uri> <midi_in_idx> <midi_out_idx>
 *                        <note> <velocity>
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv2/core/lv2.h"
#include "lv2/urid/urid.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/midi/midi.h"

#define ATOM_CAP 8192
#define NURI 64

typedef struct {
    char *s[NURI];
    LV2_URID n;
} map_t;

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri) {
    map_t *m = (map_t *)h;
    for (LV2_URID i = 0; i < m->n; i++)
        if (strcmp(m->s[i], uri) == 0) return i + 1;
    if (m->n >= NURI) return 0;
    m->s[m->n] = strdup(uri);
    return ++m->n;
}

#ifdef _WIN32
static void *open_lib(const char *path) { return (void *)LoadLibraryA(path); }
static void *sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void close_lib(void *h) { FreeLibrary((HMODULE)h); }
static const char *lib_err(void) { return "LoadLibrary/GetProcAddress failed"; }
#else
static void *open_lib(const char *path) { return dlopen(path, RTLD_NOW); }
static void *sym(void *h, const char *n) { return dlsym(h, n); }
static void close_lib(void *h) { dlclose(h); }
static const char *lib_err(void) { return dlerror(); }
#endif

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s so uri midi_in midi_out note vel\n", argv[0]);
        return 2;
    }
    const char *so = argv[1], *uri = argv[2];
    int midi_in = atoi(argv[3]), midi_out = atoi(argv[4]);
    int note = atoi(argv[5]), vel = atoi(argv[6]);

    void *h = open_lib(so);
    if (!h) { fprintf(stderr, "open: %s\n", lib_err()); return 3; }
    const LV2_Descriptor *(*getdesc)(uint32_t) =
        (const LV2_Descriptor *(*)(uint32_t))sym(h, "lv2_descriptor");
    if (!getdesc)
        getdesc = (const LV2_Descriptor *(*)(uint32_t))sym(h, "_lv2_descriptor");
    if (!getdesc) { fprintf(stderr, "no lv2_descriptor\n"); return 4; }

    const LV2_Descriptor *d = NULL;
    for (uint32_t i = 0; ; i++) {
        const LV2_Descriptor *di = getdesc(i);
        if (!di) break;
        if (strcmp(di->URI, uri) == 0) { d = di; break; }
    }
    if (!d) { fprintf(stderr, "uri not in so: %s\n", uri); return 5; }

    map_t map = {{0}, 0};
    LV2_URID_Map urid_map = { &map, map_uri };
    const LV2_Feature feat = { LV2_URID__map, &urid_map };
    const LV2_Feature *features[] = { &feat, NULL };

    LV2_Handle inst = d->instantiate(d, 48000.0, ".", features);
    if (!inst) { fprintf(stderr, "instantiate failed\n"); return 6; }

    uint8_t in_buf[ATOM_CAP], out_buf[ATOM_CAP];
    memset(in_buf, 0, sizeof in_buf);
    memset(out_buf, 0, sizeof out_buf);
    float latency = 0.0f;

    LV2_URID u_seq = map_uri(&map, LV2_ATOM__Sequence);
    LV2_URID u_frame = map_uri(&map, LV2_ATOM__frameTime);
    LV2_URID u_midi = map_uri(&map, LV2_MIDI__MidiEvent);

    LV2_Atom_Sequence *in_seq = (LV2_Atom_Sequence *)in_buf;
    in_seq->atom.type = u_seq;
    in_seq->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    in_seq->body.unit = u_frame;
    in_seq->body.pad = 0;

    uint8_t msg[3] = { (uint8_t)0x90, (uint8_t)note, (uint8_t)vel };
    LV2_Atom_Event *ev = (LV2_Atom_Event *)(
        in_buf + sizeof(LV2_Atom) + sizeof(LV2_Atom_Sequence_Body));
    ev->time.frames = 0;
    ev->body.type = u_midi;
    ev->body.size = 3;
    memcpy(LV2_ATOM_BODY(&ev->body), msg, 3);
    in_seq->atom.size += lv2_atom_pad_size((uint32_t)(sizeof(LV2_Atom_Event) + 3));

    LV2_Atom *out_atom = (LV2_Atom *)out_buf;
    out_atom->type = 0;
    out_atom->size = (uint32_t)(ATOM_CAP - sizeof(LV2_Atom));

    d->connect_port(inst, (uint32_t)midi_in, in_seq);
    d->connect_port(inst, (uint32_t)midi_out, out_buf);
    for (uint32_t p = 0; p < 8; p++) {
        if ((int)p == midi_in || (int)p == midi_out) continue;
        d->connect_port(inst, p, &latency);
    }

    if (d->activate) d->activate(inst);
    d->run(inst, 64);
    if (d->deactivate) d->deactivate(inst);

    LV2_Atom_Sequence *out_seq = (LV2_Atom_Sequence *)out_buf;
    int n_midi = 0, matched = 0;
    if (out_seq->atom.type == u_seq) {
        LV2_ATOM_SEQUENCE_FOREACH (out_seq, e) {
            if (e->body.type != u_midi || e->body.size < 3) continue;
            const uint8_t *b = (const uint8_t *)(e + 1);
            n_midi++;
            if (b[0] == 0x90 && b[1] == (uint8_t)note && b[2] == (uint8_t)vel)
                matched++;
        }
    }

    d->cleanup(inst);
    close_lib(h);
    printf("n_midi=%d matched=%d\n", n_midi, matched);
    return (n_midi == 1 && matched == 1) ? 0 : 1;
}
