/* probe_step.c -- host an exported plugin from a C process and print what
 * it does, with no Julia involved.
 *
 *   probe_step <bundle> <sample_rate> <block_size> <n_blocks> [<id> <value>]...
 *
 * reads block_size * n_blocks samples from stdin, one per line, runs them
 * through the bundle's first plugin block by block on one instance, and
 * prints the output samples one per line as %.17g. Lines starting with '#'
 * describe what the host discovered. This is how a plugin that brings its
 * own Julia runtime is checked: the Julia process running the test suite
 * already holds a libjulia, and a second one cannot be loaded into it.
 *
 * Build from the repository root:
 *   cc -O2 -o probe_step test/export/probe_step.c csrc/clap_host.c -ldl -lm
 */

#include "../../csrc/clap_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SLOTS 4

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s bundle sample_rate block_size n_blocks [id value]...\n", argv[0]);
        return 2;
    }
    const char *bundle = argv[1];
    double sr = atof(argv[2]);
    long block = atol(argv[3]);
    long nblocks = atol(argv[4]);
    double ids[SLOTS] = { -1, -1, -1, -1 }, vals[SLOTS] = { 0, 0, 0, 0 };
    for (int i = 0; i < SLOTS && 6 + 2 * i < argc; i++) {
        ids[i] = atof(argv[5 + 2 * i]);
        vals[i] = atof(argv[6 + 2 * i]);
    }

    long n = clap_host_scan(bundle);
    if (n < 0) { fprintf(stderr, "scan: %s\n", clap_host_last_error()); return 1; }
    printf("# plugins %ld\n", n);
    for (long i = 0; i < n; i++)
        printf("# plugin %s|%s\n", clap_host_scan_id(i), clap_host_scan_name(i));

    if (clap_host_open(bundle, "", sr, (double)block, 1) != 0) {
        fprintf(stderr, "open: %s\n", clap_host_last_error());
        return 1;
    }
    printf("# name %s\n", clap_host_plugin_name());
    printf("# latency %.17g\n", clap_host_latency());
    long np = clap_host_n_params();
    printf("# params %ld\n", np);
    for (long i = 0; i < np; i++)
        printf("# param %.17g|%s|%.17g|%.17g|%.17g\n", clap_host_param_id(i),
               clap_host_param_name(i), clap_host_param_min(i), clap_host_param_max(i),
               clap_host_param_default(i));

    double *buf = malloc((size_t)block * sizeof *buf);
    if (!buf) return 1;
    for (long b = 0; b < nblocks; b++) {
        for (long i = 0; i < block; i++)
            if (scanf("%lf", &buf[i]) != 1) { fprintf(stderr, "short input\n"); return 1; }
        double tok = clap_in_fill(buf, block, 1);
        double out = clap_process(tok, ids[0], vals[0], ids[1], vals[1],
                                  ids[2], vals[2], ids[3], vals[3]);
        if (isnan(out)) { fprintf(stderr, "process: %s\n", clap_host_last_error()); return 1; }
        for (long i = 0; i < block; i++)
            printf("%.17g\n", clap_out_sample(out, (double)i, 0));
    }
    free(buf);
    clap_host_close();
    return 0;
}
