# Hosting an LV2 plugin from inside a Dyad synchronous node.
#
# The LV2 counterpart of clap_io.jl, with the same shape for the same reasons.
# The implementation is csrc/lv2_host.c and the operators below are one-line
# `ccall`s into it.
#
# Named `ccall((:sym, lib), ...)` rather than Julia callbacks is what makes one
# component definition serve all three targets:
#
#   backend = :julia   Julia ccalls the shared library directly.
#   backend = :c       SynchCompiler links the library into the node's .so.
#   export_c           the emitted top.c declares
#                        extern double lv2_process(double, ...);
#                      and links against csrc/lv2_host.c with no Julia involved.
#
# The same two load-bearing properties the CLAP boundary carries:
#
#  1. Exactly one run() per tick. stkcompile does no CSE and no DCE, so one
#     equation is one call; a second call would advance the plugin's internal
#     state twice for one block of time.
#  2. Ordering. `lv2_process` takes the input block's token as its `dep`, and a
#     token that does not name the current block is refused with NaN rather than
#     answered from whatever the buffer holds.
#
# What differs from CLAP is only what LV2 itself does differently. A plugin is
# named by a URI rather than by a bundle path plus an id, because LV2 discovery
# is over a *search path* of bundle directories; the host reads the Turtle
# manifests with lilv. A parameter is a control input port and its id is the
# port index, so the ids are dense small integers rather than opaque `clap_id`s.
# A search path and a URI are strings, so they cannot cross a synchronous
# interface: they are `structural parameter`s driver-side, and `lv2_open!` must
# be called with the same ones the model was built against.

export lv2_host_available, lv2_lib_path, lv2_src_path, lv2_scan,
    lv2_open!, lv2_close!,
    lv2_is_open, lv2_last_error, lv2_plugin_name, lv2_plugin_uri,
    lv2_params, lv2_param_count, lv2_latency,
    lv2_block_size, lv2_sample_rate, lv2_channels,
    lv2_n_audio_in, lv2_n_audio_out,
    lv2_n_process, lv2_reset_counters!,
    lv2_fill!, lv2_out, lv2_test_bundle,
    LV2_WAVE_SILENCE, LV2_WAVE_SINE, LV2_WAVE_SQUARE,
    LV2_WAVE_RAMP, LV2_WAVE_IMPULSE

# The host library, exactly as for CLAP: `liblv2_host` is what `ccall` needs,
# the soname the JLL registers the library under in its `__init__`, so it is a
# compile-time constant that survives a relocated depot. The absolute path --
# what a generated C program links against -- is `lv2_lib_path()`, and the C
# source it was built from is `lv2_src_path()`.
using LV2Host_jll: LV2Host_jll

const LV2_HOST_AVAILABLE = LV2Host_jll.is_available()
# Where the JLL has no build, the soname stands in so the module still loads
# and authoring works; a hosting call then fails to load the library.
const LV2_LIB = LV2_HOST_AVAILABLE ? LV2Host_jll.liblv2_host : "liblv2_host"
const LV2_SRC = normpath(joinpath(@__DIR__, "..", "csrc", "lv2_host.c"))

# LV2 names a search path the way the platform names any path list.
const LV2_PATH_SEP = Sys.iswindows() ? ";" : ":"

"""
    lv2_lib_path() -> String

Absolute path of the prebuilt LV2 host library (from `LV2Host_jll`). This is
what a driver that links the host into a standalone program wants; Julia
callers never need it because every `ccall` here goes through [`LV2_LIB`].

To run against a locally modified `csrc/lv2_host.c` instead, build it with your
C compiler against lilv and point the JLL at it through a preference:

    using Preferences, LV2Host_jll
    set_preferences!(LV2Host_jll, "liblv2_host_path" => "/path/to/liblv2_host.so")

then restart Julia.
"""
function lv2_lib_path()
    LV2_HOST_AVAILABLE ||
        error(
        "LV2Host_jll has no build of the LV2 host for this platform " *
            "($(Base.BinaryPlatforms.host_triplet())); host from a C program over " *
            "csrc/lv2_host.c instead, see lv2_host_available()"
    )
    return LV2Host_jll.liblv2_host_path::String
end

"""
    lv2_host_available() -> Bool

Whether `LV2Host_jll` ships the prebuilt host for this platform. Where it does
not, the module loads and [`export_plugin`](@ref) works, but the `lv2_*`
hosting functions cannot load the host: host from a C program over
`csrc/lv2_host.c` instead, as `test/probe_lv2.c` does.
"""
lv2_host_available() = LV2_HOST_AVAILABLE

"""
    lv2_src_path() -> String

Path of `csrc/lv2_host.c`, which ships with the package so that a generated
standalone C program can link the host directly with no Julia present. It needs
lilv, and the three LV2 headers it includes are next to it under `csrc/vendor/`
so that such a build only has to find lilv.
"""
lv2_src_path() = LV2_SRC

"""
    LV2_WAVE_SILENCE

Waveform code for `lv2p_in_tone`: a constant zero signal.
"""
const LV2_WAVE_SILENCE = 0

"""
    LV2_WAVE_SINE

Waveform code for `amp * sin(2π * freq * t)`.

The `LV2_WAVE_*` codes are the `waveform` argument of the host's node-side tone
source, mirroring the `LV2_WAVE_*` macros in `csrc/lv2_host.h` and numbered the
same as the `CLAP_WAVE_*` family. They are integers rather than symbols because
they are arguments to a clocked equation: a test source is then described
entirely by its own parameters, with nothing to keep in sync driver-side. A
code the host does not know produces silence.
"""
const LV2_WAVE_SINE = 1

"""
    LV2_WAVE_SQUARE

Waveform code for a square wave: `±amp`, following the sign of the sine of the
same phase. See [`LV2_WAVE_SINE`](@ref) for the family.
"""
const LV2_WAVE_SQUARE = 2

"""
    LV2_WAVE_RAMP

Waveform code for `lv2p_in_tone`: a sawtooth rising from `-amplitude` to
`+amplitude` once per period.
"""
const LV2_WAVE_RAMP = 3

"""
    LV2_WAVE_IMPULSE

Waveform code for a single sample of `amp` at sample index 0 and zero
afterwards, for measuring an impulse response. See [`LV2_WAVE_SINE`](@ref) for
the family.
"""
const LV2_WAVE_IMPULSE = 4

"Join bundle directories into the path list `lv2_host_scan` and `lv2_host_open` take."
_lv2_path(dirs::AbstractVector{<:AbstractString}) = join(dirs, LV2_PATH_SEP)

"""
    lv2_test_bundle(; force = false) -> String

Build the LV2 bundle of test plugins that ships with this package
(`test/plugins/ap_test_lv2.c` and `ap_test_lv2.ttl`: `urn:audioplugins:test:gain`,
`:onepole`, `:lookahead`) and return the **directory containing** it, which is
what [`lv2_scan`](@ref) and [`lv2_open!`](@ref) take.

Hosting is only proved by hosting something, and depending on a third-party
plugin would make the suite depend on a binary whose arithmetic we cannot check
and may not be able to fetch. These are ours, and their output is analytic.

The bundle's `manifest.ttl` is written here rather than shipped, because it has
to name the binary with this platform's extension. Like
[`clap_test_bundle`](@ref) this is the one place the package needs a C
compiler, it is only needed to run the tests, and it builds into a per-package
scratch space rather than into the package directory.
"""
function lv2_test_bundle(; force::Bool = false)
    src = normpath(joinpath(@__DIR__, "..", "test", "plugins", "ap_test_lv2.c"))
    ttl = normpath(joinpath(@__DIR__, "..", "test", "plugins", "ap_test_lv2.ttl"))
    root = @get_scratch!("test_lv2-$(Sys.ARCH)")
    bundle = joinpath(root, "ap_test.lv2")
    binary = "ap_test." * (Sys.iswindows() ? "dll" : Sys.isapple() ? "dylib" : "so")
    lib = joinpath(bundle, binary)
    stale = !isfile(lib) || any(f -> stat(f).mtime > stat(lib).mtime, (src, ttl))
    if force || stale
        cc = _c_compiler()
        cc === nothing && error(
            "lv2_test_bundle: building the test plugins needs a C compiler on PATH " *
                "(tried cc, gcc, clang). Hosting itself does not: the host library comes " *
                "prebuilt from LV2Host_jll."
        )
        mkpath(bundle)
        run(`$cc $(_c_arch_flags()) -O2 -fPIC -shared -Wall -Wextra -o $lib $src`)
        cp(ttl, joinpath(bundle, "ap_test.ttl"); force = true)
        open(joinpath(bundle, "manifest.ttl"), "w") do io
            println(io, "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .")
            println(io, "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .")
            for p in ("gain", "onepole", "lookahead")
                println(
                    io, "<urn:audioplugins:test:$p> a lv2:Plugin ; ",
                    "lv2:binary <$binary> ; rdfs:seeAlso <ap_test.ttl> ."
                )
            end
        end
    end
    return root
end

# ---------------------------------------------------------------------------
# Driver-side lifecycle and discovery. Strings live here and nowhere else.
# ---------------------------------------------------------------------------

"""
    lv2_scan(lv2_path) -> Vector{@NamedTuple{uri::String, name::String}}

Enumerate the plugins in every LV2 bundle found under `lv2_path` without
instantiating anything. `lv2_path` is either a vector of bundle directories or
a single string of them joined by the platform's path separator (`:` on POSIX,
`;` on Windows); `""` means lilv's own default search path, i.e. the `LV2_PATH`
environment variable or the platform's standard directories.

Throws with the host's own message when nothing can be read, which includes a
directory that holds no bundles.
"""
function lv2_scan(lv2_path::AbstractString = "")
    n = ccall((:lv2_host_scan, LV2_LIB), Clong, (Cstring,), lv2_path)
    n < 0 && error("lv2_scan($(repr(lv2_path))) failed: $(lv2_last_error())")
    return [
        (
            uri = unsafe_string(ccall((:lv2_host_scan_uri, LV2_LIB), Cstring, (Clong,), i)),
            name = unsafe_string(ccall((:lv2_host_scan_name, LV2_LIB), Cstring, (Clong,), i)),
        )
            for i in 0:(n - 1)
    ]
end

lv2_scan(lv2_path::AbstractVector{<:AbstractString}) = lv2_scan(_lv2_path(lv2_path))

"""
    lv2_open!(lv2_path; uri = "", sample_rate = 48000, block_size = 512, channels = 1)

Instantiate and activate the plugin `uri` found under `lv2_path` (same meaning
as in [`lv2_scan`](@ref)) at a **fixed** block size: every `run()` carries
exactly `block_size` frames, which is the editing-chain contract — it must
equal the number of frames each tick carries, or the stream is not contiguous.
Pass `uri = ""` for the first plugin found.

`channels` is the number of host audio channels. The k-th audio input port
receives host channel `min(k, channels-1)`, so a mono host feeds every input of
a stereo plugin; the k-th audio output port writes host channel k, or is
discarded when `k >= channels`. A plugin with fewer than `channels` audio
output ports is refused here rather than silently half-connected.

Opening fails, loudly, for a plugin whose ports are not audio, control or
connection-optional, or which requires a host feature this host does not offer:
an unconnected required port is undefined behaviour in the LV2 specification.
"""
function lv2_open!(
        lv2_path::AbstractString; uri::AbstractString = "",
        sample_rate::Real = 48000, block_size::Integer = 512,
        channels::Integer = 1
    )
    r = ccall(
        (:lv2_host_open, LV2_LIB), Cint,
        (Cstring, Cstring, Cdouble, Cdouble, Cdouble),
        lv2_path, uri, sample_rate, block_size, channels
    )
    r == 0 || error("lv2_open!($(repr(lv2_path)); uri = $(repr(uri))) failed: $(lv2_last_error())")
    return nothing
end

lv2_open!(lv2_path::AbstractVector{<:AbstractString}; kwargs...) =
    lv2_open!(_lv2_path(lv2_path); kwargs...)

"Deactivate, free the instance and free the world. Safe when nothing is open."
function lv2_close!()
    ccall((:lv2_host_close, LV2_LIB), Cvoid, ())
    return nothing
end

"""
    lv2_last_error() -> String

Human-readable reason for the last host failure, or `""` when there has not
been one. [`lv2_open!`](@ref) and [`lv2_scan`](@ref) already fold this into the
error they throw.
"""
lv2_last_error() = unsafe_string(ccall((:lv2_host_last_error, LV2_LIB), Cstring, ()))

"""
    lv2_plugin_name() -> String

Name the open plugin's manifest gives it (`doap:name`), or an empty string when
nothing is open.
"""
lv2_plugin_name() = unsafe_string(ccall((:lv2_host_plugin_name, LV2_LIB), Cstring, ()))

"""
    lv2_plugin_uri() -> String

URI of the open plugin — its identity in LV2, and what [`lv2_open!`](@ref) was
given — or an empty string when nothing is open.
"""
lv2_plugin_uri() = unsafe_string(ccall((:lv2_host_plugin_uri, LV2_LIB), Cstring, ()))

"""
    lv2_is_open() -> Bool

Whether a plugin is currently open. A failed [`lv2_open!`](@ref) leaves this
`false`: there is no half-open state.
"""
lv2_is_open() = ccall((:lv2_host_is_open, LV2_LIB), Cdouble, ()) > 0.5

"""
    lv2_block_size() -> Int

Block size in force, in frames — the `block_size` [`lv2_open!`](@ref) was
called with. The plugin was activated under `bufsz:fixedBlockLength`, so this
is a fixed contract rather than a maximum, and it is what a caller must feed
per tick for the stream to stay contiguous.
"""
lv2_block_size() = Int(ccall((:lv2_host_block_size, LV2_LIB), Cdouble, ()))

"""
    lv2_sample_rate() -> Float64

Sample rate the open plugin was instantiated with, in Hz.
"""
lv2_sample_rate() = ccall((:lv2_host_sample_rate, LV2_LIB), Cdouble, ())

"""
    lv2_channels() -> Int

Number of host audio channels in force — the `channels` [`lv2_open!`](@ref) was
called with, and the channel count [`lv2_fill!`](@ref) and [`lv2_out`](@ref)
index into. It is a property of the host's buffers, not of the plugin: see
[`lv2_n_audio_in`](@ref) for what the plugin itself has.
"""
lv2_channels() = Int(ccall((:lv2_host_channels, LV2_LIB), Cdouble, ()))

"""
    lv2_n_audio_in() -> Int

Number of audio input ports the open plugin has. May differ from
[`lv2_channels`](@ref): a mono host feeds every input port of a stereo plugin.
"""
lv2_n_audio_in() = Int(ccall((:lv2_host_n_audio_in, LV2_LIB), Cdouble, ()))

"""
    lv2_n_audio_out() -> Int

Number of audio output ports the open plugin has. Ports beyond
[`lv2_channels`](@ref) are connected to scratch and discarded.
"""
lv2_n_audio_out() = Int(ccall((:lv2_host_n_audio_out, LV2_LIB), Cdouble, ()))

"""
    lv2_n_process() -> Int

Number of `run()` calls made since the plugin was opened, or since the last
[`lv2_reset_counters!`](@ref). Exactly one per tick is the property the tests
assert: a second call would advance the plugin's internal state twice for one
block of time.
"""
lv2_n_process() = ccall((:lv2_host_n_process, LV2_LIB), Clong, ())

"""
    lv2_param_count() -> Int

Number of control input ports the open plugin exposes, i.e.
`length(lv2_params())`.
"""
lv2_param_count() = ccall((:lv2_host_n_params, LV2_LIB), Clong, ())

"""
    lv2_latency() -> Float64

Latency the plugin writes to its designated `lv2:latency` port, in samples; `0`
when it has no such port. A plugin writes that port from `run()`, so the value
is authoritative after the first block. **Not compensated** — hosting a
lookahead plugin leaves its output shifted by this many samples relative to the
input, and a model that cares must align downstream itself.
"""
lv2_latency() = ccall((:lv2_host_latency, LV2_LIB), Cdouble, ())

"""
    lv2_reset_counters!()

Zero the host's call counters, so [`lv2_n_process`](@ref) counts from here.
Does not touch the plugin's own state.
"""
function lv2_reset_counters!()
    ccall((:lv2_host_reset_counters, LV2_LIB), Cvoid, ())
    return nothing
end

"""
    lv2_params() -> Vector{@NamedTuple{id, name, symbol, min, max, default}}

Every control input port of the open plugin. The `id`s are what a model passes
to the processing operator's slots, which is why they are numbers: an LV2 port
index is a `uint32` and every `uint32` is exactly representable as a `Float64`,
so a model can name its own parameters with nothing to keep in sync
driver-side. `symbol` is the port's `lv2:symbol`, the stable machine-readable
name a manifest guarantees; `name` is the human-readable `lv2:name`. `min`,
`max` and `default` are `NaN` where the manifest gives none.
"""
function lv2_params()
    n = lv2_param_count()
    # Written out rather than looped over a symbol: a ccall's function name and
    # library must be literal, not a local variable.
    return [
        (
            id = ccall((:lv2_host_param_id, LV2_LIB), Cdouble, (Clong,), i),
            name = unsafe_string(
                ccall(
                    (:lv2_host_param_name, LV2_LIB),
                    Cstring, (Clong,), i
                )
            ),
            symbol = unsafe_string(
                ccall(
                    (:lv2_host_param_symbol, LV2_LIB),
                    Cstring, (Clong,), i
                )
            ),
            min = ccall((:lv2_host_param_min, LV2_LIB), Cdouble, (Clong,), i),
            max = ccall((:lv2_host_param_max, LV2_LIB), Cdouble, (Clong,), i),
            default = ccall((:lv2_host_param_default, LV2_LIB), Cdouble, (Clong,), i),
        )
            for i in 0:(n - 1)
    ]
end

"The value currently connected to control input port `port_index`, or `NaN` when that index is not one."
lv2_param_value(port_index::Real) =
    ccall((:lv2_host_param_value, LV2_LIB), Cdouble, (Cdouble,), port_index)

"""
    lv2_fill!(samples; channels = 1) -> token

Fill the input block from `samples` (per channel) and return its token, so a
driver or a test can supply audio the node then processes.
"""
function lv2_fill!(samples::AbstractVector{<:Real}; channels::Integer = 1)
    v = Vector{Cdouble}(samples)
    return ccall(
        (:lv2_in_fill, LV2_LIB), Cdouble, (Ptr{Cdouble}, Clong, Clong),
        v, length(v) ÷ channels, channels
    )
end

"""
    lv2_out(token; channel = 0) -> Vector{Float64}

The output block named by `token`. Empty when the token is stale — the same
refusal the node-side accessors make, so a test cannot accidentally check
yesterday's audio.
"""
function lv2_out(token::Real; channel::Integer = 0)
    n = ccall((:lv2_out_count, LV2_LIB), Cdouble, (Cdouble,), token)
    isnan(n) && return Float64[]
    return [
        ccall(
            (:lv2_out_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble),
            token, i, channel
        ) for i in 0:(Int(n) - 1)
    ]
end

# ---------------------------------------------------------------------------
# Node-side operators. One equation, one call. Each takes the token it
# depends on, so nothing can be scheduled before the block it reads.
# ---------------------------------------------------------------------------

"""
    AudioPlugins.lv2p_in_tone(t, waveform, freq, amp) -> token

Generate one block of a test waveform ending at source time `t` seconds, fill
the input block with it on every channel, and return its token. `waveform` is a
`LV2_WAVE_*` code (see [`LV2_WAVE_SINE`](@ref)), `freq` is in Hz and `amp` in
`[0, 1]`. The alternative to [`lv2_fill!`](@ref) when the source should live
node-side: every argument is a number, so a model can be exercised with no
driver-side setup and a test can state its expected output in closed form.

Returns `NaN` when no plugin is open.
"""
lv2p_in_tone(t, waveform, freq, amp) =
    ccall(
    (:lv2_in_tone, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble, Cdouble),
    t, waveform, freq, amp
)

"""
    AudioPlugins.lv2p_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) -> token

Run the plugin over the input block named by `dep` and return the output
block's token, which [`lv2_out`](@ref) and the `lv2p_out_*` readers then take.
This is the whole of the processing path: one equation, one call.

Up to four control input ports are driven per block by the four `(id, value)`
slots, where `id` is the port index from [`lv2_params`](@ref). A negative id
means the slot is unused and a `NaN` value leaves the port as it was. An LV2
control port is a single float the plugin reads at `run()`, so — unlike CLAP's
timestamped events — a change lands on exactly the block it is passed with and
cannot be placed within one.

Returns `NaN` when nothing is open, or when `dep` does not name the *current*
input block: a stale token is refused rather than answered from whatever the
buffer still holds.
"""
lv2p_process(dep, id0, v0, id1, v1, id2, v2, id3, v3) =
    ccall(
    (:lv2_process, LV2_LIB), Cdouble,
    (
        Cdouble,                                      # dep
        Cdouble, Cdouble, Cdouble, Cdouble,           # slots 0, 1
        Cdouble, Cdouble, Cdouble, Cdouble,
    ),          # slots 2, 3
    dep, id0, v0, id1, v1, id2, v2, id3, v3
)

"""
    AudioPlugins.lv2p_out_rms(dep) -> Float64

Root-mean-square of the output block named by `dep`, over every channel.
`NaN` when `dep` is not the current output token.
"""
lv2p_out_rms(dep) = ccall((:lv2_out_rms, LV2_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.lv2p_out_peak(dep) -> Float64

Largest absolute sample in the output block named by `dep`, over every channel.
`NaN` when `dep` is not the current output token.
"""
lv2p_out_peak(dep) = ccall((:lv2_out_peak, LV2_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.lv2p_out_valid(dep) -> Float64

`1.0` when `dep` names the current output block and `0.0` when it does not, for
a model that wants to branch on freshness instead of propagating a `NaN`.
"""
lv2p_out_valid(dep) = ccall((:lv2_out_valid, LV2_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.lv2p_out_count(dep) -> Float64

Number of frames in the output block named by `dep`, i.e. the block size.
`NaN` when `dep` is not the current output token, which is how
[`lv2_out`](@ref) detects a stale token before reading any samples.
"""
lv2p_out_count(dep) = ccall((:lv2_out_count, LV2_LIB), Cdouble, (Cdouble,), dep)

"""
    AudioPlugins.lv2p_in_sample(dep, i, ch) -> Float64

Sample `i` of channel `ch` of the input block named by `dep`, zero-based in
both. `NaN` when `dep` is not the current input token.
"""
lv2p_in_sample(dep, i, ch) =
    ccall((:lv2_in_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)

"""
    AudioPlugins.lv2p_out_sample(dep, i, ch) -> Float64

Sample `i` of channel `ch` of the output block named by `dep`, zero-based in
both. `NaN` when `dep` is not the current output token — which is what makes a
stale read a visible error rather than a plausible-looking wrong answer.
[`lv2_out`](@ref) is the vector-at-a-time form.
"""
lv2p_out_sample(dep, i, ch) =
    ccall((:lv2_out_sample, LV2_LIB), Cdouble, (Cdouble, Cdouble, Cdouble), dep, i, ch)
