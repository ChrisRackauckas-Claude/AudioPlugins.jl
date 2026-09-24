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

Installing this package pulls in `X42Plugins_jll`, whose binaries are licensed
**GPL-2.0-or-later**, and loading those plugins into your Julia process means the
effective terms of the running combination are the artifact's, not this file's: if you
distribute a work that loads these plugins, GPL-2.0-or-later governs that work.

Per-component licences of what the artifact contains, read from the x42-plugins
meta-repo at pin `3fb6abe` and each submodule's own `COPYING` / source headers:

| Component | Licence | Evidence |
|---|---|---|
| this package (`src/`, `test/`) | MIT | this file |
| balance, controlfilter, matrixmixer, mididebug, midifilter, midigen, midimap, nodelay, onsettrigger, phaserotate, stepseq, stereoroute, testsignal, xfade | GPL-2.0-or-later | each `COPYING` is the GPLv2 text; headers say "either version 2 … or (at your option) any later version"; no GPL-3 source files in those trees |
| `FFTW_jll` (runtime, `phaserotate` only) | GPL-2.0-or-later | FFTW's own licence |

Not in this artifact (GPL-3.0-or-later code and/or not headless-buildable): darc, dpl,
fat1, meters, sisco, zconvo, fil4, tuna, spectra, mixtri. See the README.
