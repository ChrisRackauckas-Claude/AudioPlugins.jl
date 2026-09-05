# AudioPlugins.jl

Headless hosting of third-party audio plugins from Julia, behind a C ABI of scalar doubles —
and, in the other direction, authoring plugins from a per-sample C step function.

```julia
using AudioPlugins
bundle = clap_test_bundle()            # builds the test plugins shipped here
clap_host_open(bundle, "ap.gain", 48000, 64, 1)
```

Two formats:

| Format | Licence | State |
|---|---|---|
| **CLAP** | MIT, header-only | host implemented and tested — discovery, instantiation, parameters, block processing, latency |
| **LV2** | ISC | audio path implemented (`connect_port` / `run`); **discovery is not**, see below |
| VST3 | MIT since SDK 3.8 | not yet implemented |

## How the host is shipped

The C host (`csrc/clap_host.c`) is built by [Yggdrasil](https://github.com/JuliaPackaging/Yggdrasil)
and shipped prebuilt as `CLAPHost_jll`, so using this package needs no C toolchain
and writes nothing into the package directory. The JLL is *in addition to* `csrc/`,
not a replacement: the sources stay in the repository because a generated
standalone C program links `clap_host.c` directly, with no Julia present.
`clap_lib_path()` gives the prebuilt library's absolute path and `clap_src_path()`
the source it was built from; `CLAPHost_jll`'s version tracks the release of this
package whose `csrc/` it was built from.

The only things that need a C compiler are building the *test* plugins
(`clap_test_bundle()`), which go into a per-package scratch space, and authoring
your own with `export_plugin`. A read-only installation hosts plugins fine and
fails only at test or export time, with a message that says so.

To work on `csrc/clap_host.c` itself, build it locally and point the JLL at your
build through a preference, then restart Julia:

```julia
using Preferences, CLAPHost_jll
set_preferences!(CLAPHost_jll, "libclap_host_path" => "/path/to/libclap_host.so")
```

## Why the API looks like C rather than like Julia

The host is deliberately a thin layer over named `ccall`s into a shared library at a fixed
path, rather than an idiomatic Julia API. Its first consumer is a synchronous modelling
compiler which requires exactly that: a *named* symbol in a library at a compile-time
constant path, taking and returning scalar doubles. A process-local function pointer, a
Julia callback, or a C++ type cannot cross that boundary at all.

The upside for everyone else is that the same host serves a generated standalone C program
with no Julia present, and the cost to an ordinary Julia caller is close to zero.

## The processing contract

Every plugin format shares it, which is why one host shape fits all three:

- a **contiguous** block of samples in, a block out;
- block size **fixed at setup**;
- plugin **state persisting** across blocks;
- **scalar parameters** that may change per block.

Contiguity is the caller's responsibility: the host knows the block size and the sample
rate but cannot see your clock, so it cannot check that you are feeding it every sample
exactly once. A plugin fed a non-contiguous stream returns a perfectly valid-looking result
that simply is not continuous audio.

**Headless only.** No plugin GUI is ever loaded, and none will be.

## Testing without third-party binaries

`test/plugins/ap_test_plugins.c` is a CLAP bundle written for this repository: a gain, a
one-pole filter, and a 16-sample lookahead. Hosting is only proved by hosting something,
and depending on a third-party plugin would make the suite rest on a binary whose
arithmetic cannot be checked and may not even be fetchable. Because these are ours, every
expectation is arithmetic rather than a recording:

- `ap.gain` — output equals input times the gain, sample-exactly;
- `ap.onepole` — two consecutive blocks equal one continuous run over the concatenated
  input, which is what proves state survives block boundaries;
- `ap.lookahead` — reported latency is real and is surfaced rather than silently absorbed.

## Authoring: from a C step function to a plugin

`export_plugin` builds a plugin bundle around any C function of the shape

```c
typedef struct MyPars MyPars;
struct MyPars { double gain; /* ... */ };
typedef struct { /* state */ } my_fx_mem;
typedef struct { double y; } my_fx_out;

my_fx_out my_fx_step(double u, MyPars *pars, my_fx_mem *self);
void      my_fx_reset(my_fx_mem *self);
```

which is what a fixed-step code generator emits: a named parameter struct passed by typed
pointer, a per-instance state struct, a result struct, and `<base>_step` / `<base>_reset`.
The wrapper calls `<base>_step` once per sample per channel, with one `<base>_mem` per channel
that persists across blocks, and writes host parameter changes straight into the struct
fields the descriptor names.

The descriptor is a TOML file, and it names no code generator:

```toml
[plugin]
id = "org.example.gain"
name = "Example Gain"

[abi]
base = "my_fx"
pars = "MyPars"
sample_rate_field = "fs"      # optional: the host's rate lands in pars->fs on activate

[build]
source = "my_fx.c"
header = "my_fx.h"
pkgconfig = "my_fx.pc"        # optional: Cflags and Libs for compiling and linking source

[[param]]
id = 0
name = "Gain"
field = "gain"
min = 0.0
max = 4.0
default = 1.0
```

```julia
using AudioPlugins
spec = read_plugin_spec("my_fx.toml")
export_plugin(spec, "MyGain.clap")             # a .clap on Linux/Windows, a bundle dir on macOS
clap_open!("MyGain.clap"; block_size = 64)      # and host it, right here
```

The same package hosts what it builds, so `test/export_tests.jl` proves the seam with two
hand-written step functions under `test/export/` and no generator anywhere: a gain that is
sample-exact at 0.5, and an RBJ peaking EQ whose output matches the reference recursion,
is bitwise identical whether processed as 2 × 128 or 1 × 256 frames, and is a different,
correct filter at each of 44.1, 48 and 96 kHz because the sample rate arrives as a parameter.

What the exporter needs and does not need:

- **A C compiler** (`cc`, `gcc` or `clang` on `PATH`, or `compiler = ...`), for authoring only.
  Hosting stays toolchain-free.
- **Link flags from the `.pc` file**, not a hardcoded `-lm`: that is how libraries the
  generated C calls into reach the link line, and an undefined symbol fails the link rather
  than the first `dlopen`.
- **No licence machinery.** The output is a plain, royalty-free bundle with nothing embedded.
- **Formats register themselves.** `CLAP` ships here; a format whose SDK cannot be vendored
  publicly subtypes `PluginFormat` out of tree and calls `register_plugin_format!`.

Not yet: the output must be on the base clock — the descriptor has no way to name a
`has_<name>` presence flag, so a sub-clock output cannot be exported; there is no
`clap.state` extension, so a DAW session will not persist parameter values across reload;
Windows bundles are built but not exercised in CI.

## LV2 discovery is missing, and why

The LV2 *audio* path is genuinely simpler than CLAP's — one ISC header, ports connected
once by index, then `run(n_samples)`. Discovery is the problem: LV2 metadata lives in
Turtle/RDF manifests, which in practice means `lilv`, which needs `serd`, `sord` and
`sratom`. Julia's General registry currently has only `Serd_jll`.

So the audio path here takes the port map as explicit arguments instead. A half-correct
hand-rolled Turtle parser that silently mis-maps a port would be worse than no discovery at
all. The fix is Yggdrasil recipes for the missing JLLs, which would benefit every Julia
audio project rather than only this one.

## Known limits

- **Third-party binary code runs in-process.** A plugin that segfaults takes the Julia
  process down with it. Out-of-process hosting is the robust answer and is a much larger
  project; in-process is fine for offline work, and that is what this is for.
- **Realtime discipline is not provided.** CLAP asks a host to keep an audio thread that
  never blocks and never allocates. A garbage-collected process driving a solver that may
  retry a step cannot promise that. Harmless offline; not harmless on a live capture with a
  deadline.
- **Reported latency is surfaced, not compensated.** Callers that care must align the
  stream themselves.

## Licence

MIT. Vendored headers under `csrc/vendor/` carry their own permissive licences (CLAP: MIT;
LV2: ISC) — see `LICENSE`.
