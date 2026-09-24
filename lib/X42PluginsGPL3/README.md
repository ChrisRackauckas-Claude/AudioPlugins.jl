# X42PluginsGPL3.jl

The GPL-3.0-or-later part of Robin Gareus'
[x42-plugins](https://github.com/x42/x42-plugins) — darc, dpl, fat1 and zconvo, four
LV2 bundles and 13 plugins — as a plugin collection for
[AudioPlugins.jl](https://github.com/SciML/AudioPlugins.jl). **This package is MIT; the
binaries it installs and loads are `GPL-3.0-or-later`.** Those binaries run inside your
Julia process, so the effective terms of the running combination are the artifact's, not
this package's: if you distribute a work that loads these plugins, GPL-3.0-or-later
governs that work. The MIT licence covers only the few lines of Julia that expose the
bundle directory.

The GPL-2.0-or-later x42 plugins are a separate package,
[X42Plugins](../X42Plugins), so that using those does not bring GPLv3 code into your
process.

LV2 collections do **not** go through `register_bundle!` (that API is CLAP-only). Point
`lv2_default_path` / `lv2_scan` at `lv2_dir`:

```julia
using AudioPlugins, X42PluginsGPL3

path = lv2_default_path(lv2_dir())
lv2_scan(path)   # 13 plugins
lv2_open!(path; uri = "http://gareus.org/oss/lv2/dpl#stereo",
          sample_rate = 48000, block_size = 256, channels = 2)
```

## What is in the artifact

Four submodules at the pins of x42-plugins `3fb6abe`, built headless
(`BUILDOPENGL=no BUILDJACKAPP=no INLINEDISPLAY=no`):

| Bundle | Plugins | Opens in AudioPlugins' LV2 host |
|---|---|---|
| `darc.lv2` — dynamic compressor | `darc#mono`, `darc#stereo` | yes |
| `dpl.lv2` — digital peak limiter | `dpl#mono`, `dpl#stereo` | yes |
| `fat1.lv2` — autotune | `fat1`, `fat1#microtonal`, `fat1#scales` | yes |
| `zeroconvo.lv2` — zero-latency convolver | `zeroconvolv#{Mono,Stereo,MonoToStereo,CfgMono,CfgStereo,CfgMonoToStereo}` | no |

All ids are under `http://gareus.org/oss/lv2/`. zconvo requires `worker:schedule` and
`options:options`, which AudioPlugins' LV2 host does not provide; the six zconvo plugins
are in the artifact and listed by `lv2_scan`, but `lv2_open!` refuses them.

## Licences, component by component

Read from each submodule at the pins of meta-repo `3fb6abe`:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | `LICENSE.md` |
| darc | GPL-3.0-or-later | `COPYING` is the GPLv3 text; the source headers say "either version 2, or (at your option) any later version" |
| dpl | GPL-3.0-or-later | `src/peaklim.{cc,h}` (Fons Adriaensen) are v3-or-later |
| fat1 | GPL-3.0-or-later | `src/resampler*.{cc,h}` (zita-resampler, Fons Adriaensen) are v3-or-later, although `COPYING` is the GPLv2 text |
| zconvo | GPL-3.0-or-later | `src/zeta-convolver.{cc,h}` (a modified zita-convolver) are v3-or-later, although `COPYING` is the GPLv2 text |
| `FFTW_jll` (runtime, fat1 and zconvo) | GPL-2.0-or-later | FFTW |
| `libsndfile_jll` (runtime, zconvo) | LGPL-2.1-or-later | libsndfile |
| `libsamplerate_jll` (runtime, zconvo) | BSD-2-Clause | libsamplerate |

The remaining sources of all four plugins are GPL-2.0-or-later. zconvo's bundled
impulse response `ir/delta-48k.wav` is a unit impulse.

## Opt-in, and that is the point

AudioPlugins itself is MIT and depends on none of this. Installing AudioPlugins does not
install x42-plugins. Installing *this* package is what puts GPL-3.0-or-later binaries on
your machine.

## What the JLL contains

Four LV2 bundles under `share/lv2/` and each plugin's `COPYING` under
`share/licenses/X42PluginsGPL3/`. The recipe is `X/X42PluginsGPL3` in Yggdrasil.
