/* probe_lv2.c -- C-level verification of the LV2 host, hosting our own
 * bundle through lilv. Every expectation here is arithmetic.
 *
 *   cc -O2 -o /tmp/probe_lv2 test/probe_lv2.c csrc/lv2_host.c \
 *       -Icsrc/vendor $(pkg-config --cflags --libs lilv-0) -lm
 *   /tmp/probe_lv2 <dir containing ap_test.lv2> [<a real LV2 dir, e.g. /usr/lib/lv2>]
 *                                               [<dir containing ap_many.lv2>]
 *                                               [<dir containing ap_midi.lv2>]
 */
#include "../csrc/lv2_host.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ck(int ok, const char *what) {
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* Must match AP_MANY_LV2_N in test/plugins/ap_test_many_lv2.c. */
#define MANY_N 300

#define GAIN "urn:audioplugins:test:gain"
#define POLE "urn:audioplugins:test:onepole"
#define LOOK "urn:audioplugins:test:lookahead"

int main(int argc, char **argv) {
    const char *DIR = argc > 1 ? argv[1] : ".";

    long n = lv2_host_scan(DIR);
    ck(n == 3, "scan finds the 3 test plugins");
    if (n < 0) printf("   error: %s\n", lv2_host_last_error());
    int seen = 0;
    for (long i = 0; i < n; i++) {
        if (!strcmp(lv2_host_scan_uri(i), GAIN)) seen |= 1;
        if (!strcmp(lv2_host_scan_uri(i), POLE)) seen |= 2;
        if (!strcmp(lv2_host_scan_uri(i), LOOK)) seen |= 4;
    }
    ck(seen == 7, "  all three URIs enumerated");
    ck(strcmp(lv2_host_scan_uri(99), "") == 0, "out-of-range descriptor is empty");

    ck(lv2_host_scan("/nonexistent/lv2dir") == -1, "a directory without plugins fails");
    ck(strlen(lv2_host_last_error()) > 0, "  ... with a message");
    ck(lv2_host_open(DIR, "urn:no:such", 48000, 64, 1) != 0, "unknown URI fails");
    ck(strstr(lv2_host_last_error(), "urn:no:such") != NULL, "  ... naming the URI");
    ck(lv2_host_open(DIR, GAIN, 48000, 99999, 1) != 0, "oversized block fails");
    ck(lv2_host_open(DIR, GAIN, 48000, 64, 7) != 0, "too many channels fails");
    ck(lv2_host_open(DIR, GAIN, -1, 64, 1) != 0, "non-positive sample rate fails");
    ck(lv2_host_is_open() == 0.0, "state after a failed open is closed");

    ck(lv2_host_open(DIR, GAIN, 48000, 64, 2) == 0, "2 channels into a mono plugin opens");
    if (lv2_host_is_open() == 1.0) {
        double t = lv2_in_fill((double[64]){[0 ... 63] = 1.0}, 64, 1);
        double o = lv2_process(t, 0, 2.0, -1, 0, -1, 0, -1, 0);
        ck(lv2_out_sample(o, 0, 0) == 2.0 && lv2_out_sample(o, 63, 1) == 2.0,
           "  ... and the one output repeats on both channels");
    }
    lv2_host_close();

    ck(lv2_host_open(DIR, GAIN, 48000, 64, 1) == 0, "open gain");
    if (!lv2_host_is_open()) printf("   error: %s\n", lv2_host_last_error());
    ck(strcmp(lv2_host_plugin_name(), "AudioPlugins Test Gain") == 0, "name from the manifest");
    ck(lv2_host_n_audio_in() == 1 && lv2_host_n_audio_out() == 1, "1 audio in, 1 audio out");
    ck(lv2_host_n_params() == 1, "1 parameter (the control input)");
    ck(lv2_host_param_id(0) == 0.0, "  its id is port index 0");
    ck(strcmp(lv2_host_param_name(0), "Gain") == 0, "  named Gain");
    ck(strcmp(lv2_host_param_symbol(0), "gain") == 0, "  symbol gain");
    ck(lv2_host_param_min(0) == 0.0 && lv2_host_param_max(0) == 4.0, "  range 0..4");
    ck(lv2_host_param_default(0) == 1.0, "  default 1");
    ck(lv2_host_param_value(0) == 1.0, "  connected at its default");
    ck(isnan(lv2_host_param_value(1)), "an audio port is not a parameter");

    double tok = lv2_in_tone(1.0, LV2_WAVE_SINE, 1000.0, 0.5);
    double out = lv2_process(tok, 0, 0.5, -1, 0, -1, 0, -1, 0);
    ck(!isnan(out), "process returns a token");
    int exact = 1;
    for (long i = 0; i < 64; i++) {
        float x = (float)lv2_in_sample(tok, (double)i, 0), y = (float)lv2_out_sample(out, (double)i, 0);
        if (fabsf(x * 0.5f - y) > 1e-7f) { exact = 0; break; }
    }
    ck(exact, "out == in * 0.5, sample-exactly");
    ck(lv2_host_n_process() == 1, "exactly one run()");
    double t2 = lv2_in_fill((double[64]){[0 ... 63] = 1.0}, 64, 1);
    ck(isnan(lv2_process(tok, 0, 0.5, -1, 0, -1, 0, -1, 0)), "stale dep refused");
    double o2 = lv2_process(t2, 0, 2.0, -1, 0, -1, 0, -1, 0);
    ck(fabs(lv2_out_peak(o2) - 2.0) < 1e-6, "gain 2 on the intended block");
    ck(isnan(lv2_out_peak(out)), "superseded output token reads NaN");
    ck(lv2_host_param_value(0) == 2.0, "the port holds what was sent");
    ck(lv2_host_latency() == 0.0, "gain reports no latency");

    ck(lv2_host_open(DIR, LOOK, 48000, 64, 1) == 0, "open lookahead");
    ck(lv2_host_latency() == 16.0, "latency 16 from the designated port");
    ck(lv2_host_n_params() == 0, "no parameters");
    double imp[64] = {0}; imp[0] = 1.0;
    double ti = lv2_in_fill(imp, 64, 1);
    double oi = lv2_process(ti, -1, 0, -1, 0, -1, 0, -1, 0);
    ck(lv2_out_sample(oi, 16, 0) == 1.0 && lv2_out_sample(oi, 15, 0) == 0.0, "impulse comes out 16 later");

    /* THE test: two 32-frame blocks == one 64-frame run. */
    double split[64], whole[64], step[64];
    for (int i = 0; i < 64; i++) step[i] = 1.0;
    ck(lv2_host_open(DIR, POLE, 48000, 32, 1) == 0, "open onepole at 32");
    for (int b = 0; b < 2; b++) {
        double t = lv2_in_fill(step, 32, 1);
        double o = lv2_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0);
        for (int i = 0; i < 32; i++) split[b * 32 + i] = lv2_out_sample(o, i, 0);
    }
    ck(lv2_host_open(DIR, POLE, 48000, 64, 1) == 0, "reopen onepole at 64");
    double t64 = lv2_in_fill(step, 64, 1);
    double o64 = lv2_process(t64, 0, 0.25, -1, 0, -1, 0, -1, 0);
    for (int i = 0; i < 64; i++) whole[i] = lv2_out_sample(o64, i, 0);
    double worst = 0;
    for (int i = 0; i < 64; i++) worst = fmax(worst, fabs(split[i] - whole[i]));
    ck(worst < 1e-6, "two blocks == one concatenated run (state persists)");
    ck(fabs(split[31] - (1 - pow(0.75, 32))) < 1e-5, "step response 1-(1-a)^n at n=32");

    lv2_host_close();
    ck(lv2_host_is_open() == 0.0, "closed");
    ck(isnan(lv2_process(1, 0, 1, -1, 0, -1, 0, -1, 0)), "process after close is NaN");

    if (argc > 2) {   /* real third-party plugins, enumerated through lilv */
        long m = lv2_host_scan(argv[2]);
        printf("   %ld plugins under %s:\n", m, argv[2]);
        for (long i = 0; i < m && i < 8; i++)
            printf("     %s  (%s)\n", lv2_host_scan_uri(i), lv2_host_scan_name(i));
        ck(m > 0, "a real LV2 directory enumerates");
        int r = lv2_host_open(argv[2], "http://lv2plug.in/plugins/eg-amp", 48000, 64, 1);
        if (r == 0) {
            ck(lv2_host_n_params() == 1 && !strcmp(lv2_host_param_symbol(0), "gain"),
               "eg-amp: one control 'gain'");
            ck(lv2_host_param_min(0) == -90.0 && lv2_host_param_max(0) == 24.0,
               "eg-amp: range -90..24 dB from the manifest");
            double t = lv2_in_fill(step, 64, 1);
            double o = lv2_process(t, 0, -6.0, -1, 0, -1, 0, -1, 0);   /* -6 dB */
            ck(fabs(lv2_out_peak(o) - pow(10.0, -6.0 / 20.0)) < 1e-6, "eg-amp: -6 dB is 0.501");
        } else {
            printf("   eg-amp not opened: %s\n", lv2_host_last_error());
        }
        lv2_host_close();
    }

    if (argc > 3) {   /* a search path holding more plugins than any fixed cache */
        const char *MANY = argv[3];
        long m = lv2_host_scan(MANY);
        ck(m == MANY_N, "scan finds all 300 plugins, not a capped 256");
        if (m < 0) printf("   error: %s\n", lv2_host_last_error());

        static char seen[MANY_N];
        int unparsed = 0, repeated = 0, missing = 0;
        for (long i = 0; i < m; i++) {
            int k = -1;
            if (sscanf(lv2_host_scan_uri(i), "urn:audioplugins:test:many:%d", &k) != 1
                || k < 0 || k >= MANY_N) unparsed++;
            else if (seen[k]++) repeated++;
        }
        for (int k = 0; k < MANY_N; k++) if (!seen[k]) missing++;
        ck(unparsed == 0 && repeated == 0 && missing == 0,
           "  every one of the 300 URIs enumerated exactly once");
        if (missing) printf("   %d of %d URIs never appeared\n", missing, MANY_N);
        ck(strcmp(lv2_host_scan_uri(MANY_N), "") == 0, "  one past the end is empty");

        /* Plugin N scales by N+1, so the audio says which one was opened. */
        ck(lv2_host_open(MANY, "urn:audioplugins:test:many:299", 48000, 64, 1) == 0,
           "  open the three-hundredth plugin");
        double tm = lv2_in_fill(step, 64, 1);
        double om = lv2_process(tm, 0, 1.0, -1, 0, -1, 0, -1, 0);
        ck(fabs(lv2_out_peak(om) - 300.0) < 1e-3, "  and it is the one that scales by 300");

        ck(lv2_host_scan(DIR) == 3, "  rescanning the small bundle finds 3 again");
        ck(strcmp(lv2_host_scan_uri(3), "") == 0, "  with nothing left over from the big one");
        lv2_host_close();
    }

    if (argc > 4) {   /* the MIDI bundle: required atom ports */
        const char *MDIR = argv[4];
        const char *NG = "urn:audioplugins:test:notegain";

        ck(lv2_host_open(MDIR, NG, 48000, 64, 1) == 0,
           "open notegain (required atom ports)");
        if (!lv2_host_is_open()) printf("   error: %s\n", lv2_host_last_error());
        ck(lv2_host_n_atom_ports() == 3.0, "three atom ports discovered");
        ck(lv2_host_atom_port_index(0) == 0.0 && lv2_host_atom_port_is_input(0) == 1.0,
           "  slot 0 is atom input port 0 (midi_in)");
        ck(lv2_host_atom_port_index(1) == 1.0 && lv2_host_atom_port_is_input(1) == 0.0,
           "  slot 1 is atom output port 1 (midi_out)");
        ck(lv2_host_atom_port_index(2) == 5.0 && lv2_host_atom_port_is_input(2) == 1.0,
           "  slot 2 is atom input port 5 (patch_in)");
        ck(lv2_host_atom_port_midi(0) == 1.0 && lv2_host_atom_port_midi(1) == 1.0
           && lv2_host_atom_port_midi(2) == 0.0,
           "  MidiEvent declared for the MIDI ports, not patch_in");
        ck(lv2_host_atom_port_size(0) == 16384.0, "  rsz:minimumSize honoured");
        ck(lv2_host_atom_port_size(1) == LV2_HOST_ATOM_BUF, "  undeclared gets the floor");
        ck(lv2_host_n_params() == 0, "a control output is not a parameter");

        ck(isnan(lv2_in_midi(0, 0, 0x90, 60, 100)), "midi event before a block is refused");

        /* Gate timing: note-on at frame 16 opens the gate; a control change
         * at frame 32 rides along and is echoed but changes nothing. */
        double t = lv2_in_fill(step, 64, 1);
        ck(lv2_in_midi(0, 16, 0x90, 60, 127) == 1.0, "note-on queued");
        ck(lv2_in_midi(0, 32, 0xB0, 1, 2) == 2.0, "a second event queues");
        ck(isnan(lv2_in_midi(0, 8, 0x90, 60, 100)), "an out-of-order event is refused");
        ck(isnan(lv2_in_midi(0, 64, 0x90, 60, 100)), "frame == block size is refused");
        ck(isnan(lv2_in_midi(2, 0, 0x90, 60, 100)), "an audio port refuses MIDI");
        ck(isnan(lv2_in_midi(1, 0, 0x90, 60, 100)), "an atom output refuses MIDI");
        ck(isnan(lv2_in_midi(5, 0, 0x90, 60, 100)), "a non-MIDI atom input refuses MIDI");
        double o = lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0);
        int gate = 1;
        for (long i = 0; i < 64; i++) {
            double want = (i >= 16) ? 1.0 : 0.0;
            if (fabs(lv2_out_sample(o, (double)i, 0) - want) > 1e-7) { gate = 0; break; }
        }
        ck(gate, "note-on at 16 opens the gate sample-exactly");
        ck(lv2_host_port_value(4) == 2.0, "2 events echoed to midi_out");
        ck(isnan(lv2_host_param_value(4)), "  n_echo is not a parameter");
        ck(isnan(lv2_host_port_value(2)), "  an audio port has no port value");

        /* A block with no events: the gate stays open at velocity 127/127. */
        double t2 = lv2_in_fill(step, 64, 1);
        double o2 = lv2_process(t2, -1, 0, -1, 0, -1, 0, -1, 0);
        ck(lv2_out_sample(o2, 0, 0) == 1.0 && lv2_out_sample(o2, 63, 0) == 1.0,
           "gate held across a silent block");
        ck(lv2_host_port_value(4) == 0.0, "  and nothing was echoed");

        /* note-off at 0 closes it for the whole block ... */
        double t3 = lv2_in_fill(step, 64, 1);
        lv2_in_midi(0, 0, 0x80, 60, 0);
        double o3 = lv2_process(t3, -1, 0, -1, 0, -1, 0, -1, 0);
        ck(lv2_out_peak(o3) == 0.0, "note-off at 0 silences the block");

        /* ... and a note-on at velocity 64 reopens it at gain 64/127. */
        double t4 = lv2_in_fill(step, 64, 1);
        lv2_in_midi(0, 0, 0x90, 60, 64);
        double o4 = lv2_process(t4, -1, 0, -1, 0, -1, 0, -1, 0);
        ck(fabs(lv2_out_sample(o4, 0, 0) - 64.0 / 127.0) < 1e-6,
           "note-on velocity 64 gives gain 64/127");
        lv2_host_close();
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
