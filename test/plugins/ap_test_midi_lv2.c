/* ap_test_midi_lv2.c -- the MIDI half of the LV2 test fixtures: a plugin
 * that requires an atom port, for proving the host carries them. Same
 * rule as ap_test_lv2.c: every expectation in the probes is arithmetic.
 *
 *   urn:audioplugins:test:notegain
 *   ports: 0 midi_in  (atom in, required, supports MidiEvent,
 *                      rsz:minimumSize 512)
 *          1 midi_out (atom out, required: the input echoed back)
 *          2 in, 3 out (audio)
 *          4 n_echo   (ctrl out: events echoed this block)
 *          5 patch_in (atom in, connectionOptional, supports patch:Message)
 *
 * A note-on opens the gate at the event's exact frame and sets the gain to
 * velocity/127; a note-off or a zero-velocity note-on closes it. So
 * out[i] = in[i] * velocity/127 while a note is held and 0 otherwise -- an
 * event that lands one sample early or late is visible in the output,
 * which is what the probes assert.
 *
 * n_echo is how the atom OUTPUT path is proved through a scalar: the
 * plugin reads the capacity the host wrote in midi_out->atom.size, clears
 * the sequence and echoes each input event back; a host that failed to
 * reset the capacity each run would present the previous block's content
 * size instead, and the count drops below the number of events sent.
 */
#include "../../csrc/vendor/lv2/core/lv2.h"
#include "../../csrc/vendor/lv2/urid/urid.h"
#include "../../csrc/vendor/lv2/atom/atom.h"
#include "../../csrc/vendor/lv2/atom/util.h"
#include "../../csrc/vendor/lv2/midi/midi.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const LV2_Atom_Sequence *midi_in;
    LV2_Atom_Sequence       *midi_out;
    const float *in;
    float       *out;
    float       *n_echo;
    LV2_URID     u_sequence, u_frametime, u_midievent;
    float        gain;          /* velocity/127 while a note is held */
} inst_t;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                              const char *bundle, const LV2_Feature *const *f) {
    (void)d; (void)rate; (void)bundle;
    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    for (int i = 0; f && f[i]; i++) {
        if (strcmp(f[i]->URI, LV2_URID__map) == 0) {
            const LV2_URID_Map *m = (const LV2_URID_Map *)f[i]->data;
            s->u_sequence  = m->map(m->handle, LV2_ATOM__Sequence);
            s->u_frametime = m->map(m->handle, LV2_ATOM__frameTime);
            s->u_midievent = m->map(m->handle, LV2_MIDI__MidiEvent);
        }
    }
    return s;
}
static void cleanup(LV2_Handle h) { free(h); }
static const void *extension_data(const char *uri) { (void)uri; return NULL; }

static void connect_notegain(LV2_Handle h, uint32_t port, void *data) {
    inst_t *s = (inst_t *)h;
    switch (port) {
    case 0: s->midi_in  = (const LV2_Atom_Sequence *)data; break;
    case 1: s->midi_out = (LV2_Atom_Sequence *)data; break;
    case 2: s->in       = (const float *)data; break;
    case 3: s->out      = (float *)data; break;
    case 4: s->n_echo   = (float *)data; break;
    default: break;
    }
}
static void activate_notegain(LV2_Handle h) {
    ((inst_t *)h)->gain = 0.0f;
}
static void deactivate(LV2_Handle h) { (void)h; }

static void run_notegain(LV2_Handle h, uint32_t n) {
    inst_t *s = (inst_t *)h;
    uint32_t i = 0;
    int echoed = 0;

    /* The host presents the output buffer's capacity in atom.size. Take
     * it, then re-head the buffer to an empty sequence. */
    uint32_t cap = 0;
    if (s->midi_out) {
        cap = s->midi_out->atom.size;
        s->midi_out->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
        s->midi_out->atom.type = s->u_sequence;
        s->midi_out->body.unit = s->u_frametime;
        s->midi_out->body.pad  = 0;
    }

    if (s->midi_in && s->midi_in->atom.type == s->u_sequence) {
        LV2_ATOM_SEQUENCE_FOREACH (s->midi_in, ev) {
            while (i < n && (int64_t)i < ev->time.frames) {
                s->out[i] = s->in ? s->in[i] * s->gain : 0.0f;
                i++;
            }
            if (ev->body.type == s->u_midievent && ev->body.size >= 1) {
                const uint8_t *msg = (const uint8_t *)(ev + 1);
                switch (lv2_midi_message_type(msg)) {
                case LV2_MIDI_MSG_NOTE_ON:
                    s->gain = (ev->body.size >= 3 && msg[2] != 0)
                              ? (float)msg[2] / 127.0f : 0.0f;
                    break;
                case LV2_MIDI_MSG_NOTE_OFF: s->gain = 0.0f; break;
                default: break;
                }
            }
            if (s->midi_out) {
                LV2_Atom_Event *dst = lv2_atom_sequence_end(
                    &s->midi_out->body, s->midi_out->atom.size);
                uint32_t total = (uint32_t)sizeof(*dst) + ev->body.size;
                if ((uint8_t *)dst + lv2_atom_pad_size(total) <=
                    (uint8_t *)s->midi_out + cap) {
                    memcpy(dst, ev, total);
                    s->midi_out->atom.size += lv2_atom_pad_size(total);
                    echoed++;
                }
            }
        }
    }
    for (; i < n; i++) s->out[i] = s->in ? s->in[i] * s->gain : 0.0f;
    if (s->n_echo) *s->n_echo = (float)echoed;
}

static const LV2_Descriptor DESCS[] = {
    { "urn:audioplugins:test:notegain", instantiate, connect_notegain,
      activate_notegain, run_notegain, deactivate, cleanup, extension_data },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index < 1) ? &DESCS[index] : NULL;
}
