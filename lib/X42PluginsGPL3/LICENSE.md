MIT License

Copyright (c) 2026 JuliaHub, Inc. and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


---

## What this licence covers, and what it does not

The MIT licence above covers **this package only**: the Julia source under `src/`, which
is the handful of lines that expose the LV2 bundle directory to AudioPlugins. It covers
no binary.

Installing this package pulls in `X42PluginsGPL3_jll`, whose binaries are licensed
**GPL-3.0-or-later**, and loading those plugins into your Julia process means the
effective terms of the running combination are the artifact's, not this file's: if you
distribute a work that loads these plugins, GPL-3.0-or-later governs that work.

Per-component licences of what the artifact contains, read from each submodule's own
`COPYING` and source headers at the pins of the x42-plugins meta-repo `3fb6abe`:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | this file |
| darc | GPL-3.0-or-later | `COPYING` is the GPLv3 text; `src/lv2.c` says "either version 2, or (at your option) any later version" |
| dpl | GPL-3.0-or-later | `COPYING` is the GPLv3 text; `src/peaklim.{cc,h}` (Fons Adriaensen) say "version 3 … or (at your option) any later version"; the rest is v2-or-later |
| fat1 | GPL-3.0-or-later | `COPYING` is the GPLv2 text, but `src/resampler*.{cc,h}` (zita-resampler, Fons Adriaensen) are v3-or-later; the rest is v2-or-later |
| zconvo | GPL-3.0-or-later | `COPYING` is the GPLv2 text, but `src/zeta-convolver.{cc,h}` (a modified zita-convolver) are v3-or-later; the rest is v2-or-later |
| `FFTW_jll` (runtime, fat1 and zconvo) | GPL-2.0-or-later | FFTW's own licence |
| `libsndfile_jll` (runtime, zconvo) | LGPL-2.1-or-later | libsndfile's own licence |
| `libsamplerate_jll` (runtime, zconvo) | BSD-2-Clause | libsamplerate's own licence |

GPL-2.0-or-later, LGPL-2.1-or-later and BSD-2-Clause code all combine with
GPL-3.0-or-later code as GPL-3.0-or-later, which is what the artifact as a whole is. The
artifact carries each plugin's `COPYING` under `share/licenses/X42PluginsGPL3/`.
