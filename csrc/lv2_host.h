/* lv2_host.h -- hosting an LV2 plugin, audio path only. See lv2_host.c for
 * why discovery is deliberately absent: an LV2 port map lives in Turtle/RDF
 * manifests, not in the binary, so the caller supplies it explicitly here. */
#ifndef LV2_HOST_H
#define LV2_HOST_H
#ifdef __cplusplus
extern "C" {
#endif

/* Driver-side: strings and enumeration. */
long        lv2_host_scan(const char *path);      /* descriptor count, -1 on failure */
const char *lv2_host_scan_uri(long i);
const char *lv2_host_last_error(void);
const char *lv2_host_uri(void);
void        lv2_host_close(void);

/* Instantiate, connect the given ports, activate. `in_port`/`out_port` are
 * audio port INDICES and `ctrl_ports` control port indices -- read them from
 * the plugin's .ttl, because this host does not parse RDF. Returns 0 on
 * success. */
int lv2_host_open(const char *path, const char *uri, double sample_rate,
                  double block_size, double in_port, double out_port,
                  const double *ctrl_ports, long n_ctrl);

double lv2_host_is_open(void);
double lv2_host_block_size(void);
long   lv2_host_n_run(void);

/* The block, and the node-side operator. Same dep-threading as CLAP: a token
 * that does not name the current block is refused with NaN. */
double lv2_in_fill(const double *samples, long n);
double lv2_run(double dep, double c0, double c1, double c2, double c3);
double lv2_out_sample(double dep, double i);
double lv2_out_rms(double dep);
double lv2_out_peak(double dep);

#ifdef __cplusplus
}
#endif
#endif /* LV2_HOST_H */
