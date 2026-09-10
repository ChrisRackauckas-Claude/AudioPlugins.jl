# Authoring a plugin

[`export_plugin`](@ref) is the inverse of the host: it builds a plugin bundle around a
**per-sample step function**, so that something which already computes one output sample
from one input sample can be loaded by a DAW.

The interface is deliberately "a step function plus a descriptor", and it names no code
generator. Anything that can emit

```c
<base>_out <base>_step(<inputs...>, <Pars> *pars, <base>_mem *self);
void       <base>_reset(<base>_mem *self);
```

can be built into a plugin: a named parameter struct passed by typed pointer, a
per-instance state struct, a result struct, and a reset that clears the state. That is
what a fixed-step code generator emits, and it is all the exporter assumes.

The generated wrapper calls `<base>_step` once per sample per channel, with one
`<base>_mem` per channel that persists across blocks, and writes host parameter changes
straight into the struct fields the descriptor names.

## The three pieces

  1. A **step function**, as either [`CStep`](@ref) (C source plus a header) or
     [`JuliaStep`](@ref) (Julia `Base.@ccallable` functions compiled by juliac).
  2. A **descriptor** — a [`PluginSpec`](@ref), usually read from a TOML file with
     [`read_plugin_spec`](@ref) — saying what the plugin is called, what its ABI base
     name is, and which struct fields are parameters.
  3. A **format**, [`CLAP`](@ref) by default.

```julia
using AudioPlugins
spec = read_plugin_spec("my_fx.toml")
export_plugin(spec, "MyGain.clap")
```

The output is a plain, royalty-free bundle with no licence machinery embedded in it: a
`.clap` shared object on Linux and Windows, a `Name.clap/Contents/MacOS/Name` directory
on macOS. On Linux and macOS a C-step bundle can be hosted from the same session that
built it — [`clap_open!`](@ref) will load it like any other plugin.

Authoring needs a C compiler (`cc`, `gcc` or `clang` on `PATH`, or `compiler = "..."`).
Hosting does not.

## A C step

```c
/* my_fx.h */
typedef struct { double gain; } MyPars;
typedef struct { double unused; } my_fx_mem;
typedef struct { double y; } my_fx_out;

my_fx_out my_fx_step(double u, MyPars *pars, my_fx_mem *self);
void      my_fx_reset(my_fx_mem *self);
```

The wrapper C is compiled under `-Wall -Wextra -Werror`; the step's own C is not, since
generated C tends to carry harmless unused temporaries. Both are built with hidden
visibility, so two plugins that happen to share an ABI base name can coexist in one host
process. Link flags come from the descriptor's `pkgconfig` file rather than a hardcoded
`-lm` — that is how libraries the generated C calls into reach the link line — and an
undefined symbol fails the link rather than the first `dlopen`.

## A Julia step

[`JuliaStep`](@ref) compiles Julia `Base.@ccallable` functions over isbits structs into a
trimmed shared library with [juliac](https://github.com/JuliaLang/JuliaC.jl), and the
wrapper and the Julia image become one bundle:

```julia
struct GainPars; gain::Float64; bypass::Bool; end
struct GainMem;  ticks::Int64; end
struct GainOut;  y::Float64; end

Base.@ccallable function my_fx_step(u::Float64, pars::Ptr{GainPars}, self::Ptr{GainMem})::GainOut
    p = unsafe_load(pars)
    return GainOut(p.bypass ? u : p.gain * u)
end
Base.@ccallable function my_fx_reset(self::Ptr{GainMem})::Cvoid
    unsafe_store!(self, GainMem(0))
    return nothing
end
```

The parameter, state and output structs are read off the `@ccallable` signature and
declared to C by a generated header — with `_Static_assert`s of every size and offset —
so nothing about the layout is written twice, and the descriptor needs no `pars` name for
a Julia step.

This path needs `using JuliaC` and Julia ≥ 1.12; JuliaC is a weak dependency and the
`AudioPluginsJuliaCExt` extension does the build. Without it, [`export_plugin`](@ref)
raises an error saying so.

### What a Julia step brings with it

A juliac-built plugin is pure native code for the step itself, but it links `libjulia` and
initialises a Julia runtime when the host loads it.

  - **Where the runtime is found.** By default the plugin's rpath points at the absolute
    path of the Julia that built it, which runs on that machine only.
    `JuliaStep(bundle = true)` copies the runtime (about 120 MB of libraries) next to the
    plugin — `Name.clap.runtime/` beside a Linux `.clap`, `Contents/Resources/julia/`
    inside a macOS bundle — with a relative rpath, and that is the relocatable form.
  - **Windows has no rpath.** The loader resolves a DLL's imports from the host
    executable's directory, the system directories and `PATH`, never from the DLL's own
    directory unless it was opened with `LOAD_WITH_ALTERED_SEARCH_PATH`, which a DAW does
    not do. So on Windows a Julia step must be bundled: the plugin DLL and the runtime go
    together into `Name.clap.runtime\bin`, and `Name.clap` is a small shim
    (`csrc/clap_forward_shim.c`, with no Julia in it) whose `clap_entry` loads the real
    plugin from beside itself with that flag, puts the same directory at the front of the
    process `PATH` for the libraries Julia opens by name at start-up, and forwards
    `get_factory` and `deinit`.
  - **One runtime per process, unless privatised.** Two juliac plugins in one host would
    share, and fight over, one `libjulia`. `JuliaStep(bundle = true, privatize = true)`
    salts the bundled runtime's library names and symbol versions so each plugin loads its
    own. JuliaC salts on Linux and macOS only; on Windows `privatize` is refused with a
    pointer to [JuliaC.jl #186](https://github.com/JuliaLang/JuliaC.jl/issues/186).
  - **A juliac plugin cannot be hosted from inside the Julia process that built it.** The
    test suite hosts them from C probes in a separate process.
  - **Realtime.** JuliaC disables Julia's signal handlers and pins the runtime to one
    thread for a library, and an isbits step allocates nothing, but the garbage collector
    still exists in the audio callback. A C step has no such caveat. See
    [No realtime discipline](@ref).

Not yet: privatised (coexisting) Julia steps on Windows; Julia-step bundling is tested on
Linux and Windows, not on macOS.

## The TOML descriptor

[`read_plugin_spec`](@ref) reads a descriptor from TOML. Relative paths in `[build]`
resolve against the file's directory, and the format is versioned by `schema` — only `1`
exists.

```toml
[plugin]
id = "org.example.gain"
name = "Example Gain"

[abi]
base = "my_fx"                # my_fx_step, my_fx_reset, my_fx_mem, my_fx_out
pars = "MyPars"               # the C header's parameter struct (C step only)
sample_rate_field = "fs"      # optional: the host's rate lands in pars->fs on activate

[build]
source = "my_fx.c"            # a C step ...
header = "my_fx.h"
pkgconfig = "my_fx.pc"        # optional: Cflags and Libs for compiling and linking source
# julia = "my_fx.jl"          # ... or a Julia step, with optional project, trim, bundle

[[param]]
id = 0
name = "Gain"
field = "gain"
min = 0.0
max = 4.0
default = 1.0
```

Every key, including the optional ones, is listed in [`read_plugin_spec`](@ref)'s
docstring; the same fields are available directly on the [`PluginSpec`](@ref)
constructor, and the parameter entries are [`PluginParam`](@ref)s.

A value arriving from the host is clamped to `[min, max]`, rounded when the parameter is
`stepped`, and converted to the field's `ctype` — `"double"`, `"bool"` or `"int64_t"`.
Fields of the parameter struct that are *not* plugin parameters can be fixed once at
instantiation through the `[constants]` table.

## Outputs on a slower clock

An output produced on a clock slower than the sample clock is declared with
`sub_clock = true`. `<base>_out` then also carries a `bool has_<output>` presence flag,
and on a sample where it is false the wrapper holds the last present value per channel
(zero until the first). The phase lives in the step's own state, so it carries across
blocks like everything else.

## State

The wrapper implements `clap.state`, so a DAW session reloads with the parameter values it
was saved with: a small little-endian blob of `(id, value)` pairs. `test/export/probe_state.c`
is a minimal host that saves, loads into a fresh instance, and offers garbage.

## Adding a format

[`CLAP`](@ref) is the only format that ships here, because it is the only one whose SDK
can be vendored in a public repository under a permissive licence. A format whose SDK
cannot subtypes [`PluginFormat`](@ref) out of tree and calls
[`register_plugin_format!`](@ref); [`plugin_format`](@ref) then looks it up by name.

The five methods a format implements are listed in [`PluginFormat`](@ref)'s docstring, and
the CLAP implementations of them —
[`AudioPlugins.emit_wrapper`](@ref), [`AudioPlugins.place_library`](@ref) and
[`AudioPlugins.runtime_layout`](@ref) — are the worked example.
