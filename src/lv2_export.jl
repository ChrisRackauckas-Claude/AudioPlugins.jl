# Authoring LV2: the format seam of plugin_export.jl, implemented for LV2.
#
# What makes LV2 different from CLAP here is that the plugin's metadata is
# not in the binary. A bundle is a directory holding the shared object and
# the Turtle that describes it, and everything a host knows before
# instantiating -- the plugin's URI, its ports, their classes, symbols,
# indices and ranges -- it reads from that Turtle. Emitting it is therefore
# the substance of this file, and the LV2 authoring tests round-trip every
# declaration back out through lilv rather than trusting the text.

export LV2

"""
    LV2()

The LV2 plugin format (ISC; the headers are vendored under `csrc/vendor/lv2`).
A bundle is a directory named `Name.lv2` holding the plugin binary, a
`manifest.ttl` naming it, and a `Name.ttl` describing the ports —
the same shape on every platform.

The plugin is named by URI, not by the bundle's filename; see
[`lv2_plugin_uri`](@ref). Only a [`CStep`](@ref) can be built into an LV2
bundle: a [`JuliaStep`](@ref) is refused, because a bundled Julia runtime
has no LV2 layout yet.
"""
struct LV2 <: PluginFormat end

format_name(::LV2) = "lv2"
bundle_extension(::LV2) = ".lv2"

# ---------------------------------------------------------------------------
# Naming: the plugin URI
# ---------------------------------------------------------------------------

const URI_SCHEME = r"^[A-Za-z][A-Za-z0-9+.\-]*:"
const URN_NSS_SAFE = r"^[A-Za-z0-9._~:\-]+$"

"""
    lv2_plugin_uri(spec::PluginSpec) -> String

The URI the plugin described by `spec` is published under, which in LV2 *is*
its identity: a host finds it by URI and never by bundle or file name. Derived
from the descriptor's `id`, in one of two ways:

  * an `id` that is already an absolute URI — anything with a scheme, such as
    `"http://example.org/plugins/gain"` — is the URI, verbatim. This is how an
    author publishes under a domain they control, and it costs nothing on the
    other formats, whose ids are free-form strings;
  * otherwise the `id` is a plain name (reverse-DNS by convention, as CLAP
    wants) and the URI is `"urn:audioplugins:" * id`, e.g.
    `urn:audioplugins:org.example.gain`.

The mapping is total and injective, so two descriptors collide as LV2 plugins
exactly when they collide as CLAP plugins. An `id` that is neither a URI nor
usable unescaped in a URN is rejected rather than escaped, so that the URI in
the bundle is always the one the descriptor can be read to say.
"""
function lv2_plugin_uri(spec::PluginSpec)
    id = spec.id
    occursin(URI_SCHEME, id) && return _check_uri_chars(id, "plugin id")
    occursin(URN_NSS_SAFE, id) || throw(
        ArgumentError(
            "plugin id $(repr(id)) cannot be used as an LV2 URI: it is not an absolute " *
                "URI, and as a URN name it may only contain letters, digits and " *
                "`. _ ~ - :`. Give the descriptor an absolute URI as its id."
        )
    )
    return "urn:audioplugins:" * id
end

"""
    _check_uri_chars(s, what) -> String

`s` unchanged if it can go into a Turtle `<...>` as it stands. Turtle forbids
`<`, `>`, `"`, `{`, `}`, `|`, `^`, `` ` ``, `\\` and anything below `0x21` in an
IRI reference, and escaping them silently would publish a URI that is not the
one the descriptor names.
"""
function _check_uri_chars(s::AbstractString, what::AbstractString)
    for c in s
        (c in "<>\"{}|^`\\" || c <= '\x20') && throw(
            ArgumentError(
                "$what $(repr(s)) contains $(repr(c)), which cannot appear in a URI"
            )
        )
    end
    return String(s)
end

# ---------------------------------------------------------------------------
# The port layout, which is the bundle's public contract
# ---------------------------------------------------------------------------

const LV2Port = @NamedTuple{
    index::Int, input::Bool, audio::Bool, symbol::String, name::String,
    range::Union{Nothing, NTuple{3, Float64}}, latency::Bool,
}

"""
    lv2_ports(spec::PluginSpec) -> Vector{LV2Port}

The ports the bundle declares, in index order — the layout
`csrc/lv2_plugin_template.c` is written against and the generated Turtle
publishes:

  1. one control input per descriptor parameter, in **descriptor order**;
  2. `spec.channels` audio inputs, symbols `in_0`, `in_1`, …;
  3. `spec.channels` audio outputs, symbols `out_0`, `out_1`, …;
  4. when `spec.latency > 0`, one control output `latency` carrying
     `lv2:designation lv2:latency`.

A parameter's symbol is the parameter struct field it writes, which the
descriptor already requires to be a C identifier and so is a valid LV2 symbol.
Its **port index is its position in the descriptor, not its `id`**: LV2 port
indices are contiguous from zero, so a descriptor's sparse parameter ids
cannot survive into them, and this package's LV2 host reports the port index
as the parameter id.
"""
function lv2_ports(spec::PluginSpec)
    ports = LV2Port[]
    add(input, audio, symbol, name, range, latency) =
        push!(ports, (; index = length(ports), input, audio, symbol, name, range, latency))
    for p in spec.params
        add(true, false, p.field, p.name, (p.min, p.max, p.default), false)
    end
    for c in 1:spec.channels
        add(true, true, "in_$(c - 1)", "In $c", nothing, false)
    end
    for c in 1:spec.channels
        add(false, true, "out_$(c - 1)", "Out $c", nothing, false)
    end
    if spec.latency > 0
        add(false, false, "latency", "Latency", nothing, true)
    end
    symbols = [p.symbol for p in ports]
    allunique(symbols) || throw(
        ArgumentError(
            "LV2 port symbols must be unique within a plugin, got $(symbols). A " *
                "parameter's symbol is the struct field it writes, so rename the field " *
                "that collides with an audio or latency port."
        )
    )
    return ports
end

# ---------------------------------------------------------------------------
# Turtle
# ---------------------------------------------------------------------------

"""
    _ttl_string(s) -> String

`s` as a Turtle string literal, with the escapes Turtle defines. A control
character with no escape is refused rather than written raw.
"""
function _ttl_string(s::AbstractString)
    io = IOBuffer()
    print(io, '"')
    for c in s
        if c == '"' || c == '\\'
            print(io, '\\', c)
        elseif c == '\n'
            print(io, "\\n")
        elseif c == '\r'
            print(io, "\\r")
        elseif c == '\t'
            print(io, "\\t")
        elseif c <= '\x1f' || c == '\x7f'
            throw(ArgumentError("control character $(repr(c)) in Turtle string $(repr(s))"))
        else
            print(io, c)
        end
    end
    print(io, '"')
    return String(take!(io))
end

"""
    _ttl_number(x) -> String

`x` as a Turtle numeric literal. Turtle's own grammar for a decimal or double
is checked here, because a literal lilv cannot read is a port that silently
loses its range.
"""
function _ttl_number(x::Real)
    isfinite(x) || throw(ArgumentError("cannot write non-finite $x as a Turtle number"))
    s = repr(Float64(x))
    occursin(r"^[+-]?(\d+\.\d*([eE][+-]?\d+)?|\d+[eE][+-]?\d+)$", s) ||
        throw(ArgumentError("$(repr(s)) is not a Turtle decimal or double literal"))
    return s
end

"""
    _ttl_relative_iri(name) -> String

A bundle-relative file name as a Turtle IRI reference. Relative IRIs in a
bundle resolve against the file they are written in, which is how a manifest
names a binary it sits beside.
"""
_ttl_relative_iri(name::AbstractString) = "<" * _check_uri_chars(name, "bundle file name") * ">"

function _lv2_port_block(p::LV2Port)
    io = IOBuffer()
    classes = string(
        p.input ? "lv2:InputPort" : "lv2:OutputPort", ", ",
        p.audio ? "lv2:AudioPort" : "lv2:ControlPort"
    )
    println(io, "[")
    println(io, "        a ", classes, " ;")
    print(
        io, "        lv2:index ", p.index, " ; lv2:symbol ", _ttl_string(p.symbol),
        " ; lv2:name ", _ttl_string(p.name)
    )
    if p.range !== nothing
        lo, hi, def = p.range
        print(
            io, " ;\n        lv2:minimum ", _ttl_number(lo), " ; lv2:maximum ",
            _ttl_number(hi), " ; lv2:default ", _ttl_number(def)
        )
    end
    p.latency && print(
        io, " ;\n        lv2:designation lv2:latency ; lv2:portProperty lv2:reportsLatency"
    )
    print(io, "\n    ]")
    return String(take!(io))
end

"""
    lv2_turtle(spec::PluginSpec; binary, seealso) -> (; manifest::String, plugin::String)

The two Turtle documents of the bundle for `spec`: the `manifest.ttl` that
points a host at the binary named `binary` and the description in `seealso`,
and that description itself. Everything a host knows about the plugin before
it loads anything is in here, so `lv2_ports` is the single source both this
and the rendered C wrapper are written from.
"""
function lv2_turtle(spec::PluginSpec; binary::AbstractString, seealso::AbstractString)
    uri = "<" * lv2_plugin_uri(spec) * ">"
    manifest = IOBuffer()
    println(manifest, "# Generated by AudioPlugins.jl. Do not edit.")
    println(manifest, "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .")
    println(manifest, "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .")
    println(manifest)
    println(manifest, uri)
    println(manifest, "    a lv2:Plugin ;")
    println(manifest, "    lv2:binary ", _ttl_relative_iri(binary), " ;")
    println(manifest, "    rdfs:seeAlso ", _ttl_relative_iri(seealso), " .")

    plugin = IOBuffer()
    println(plugin, "# Generated by AudioPlugins.jl from the plugin descriptor. Do not edit.")
    println(plugin, "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .")
    println(plugin, "@prefix doap: <http://usefulinc.com/ns/doap#> .")
    println(plugin, "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .")
    println(plugin)
    println(plugin, uri)
    println(plugin, "    a lv2:Plugin ;")
    println(plugin, "    doap:name ", _ttl_string(spec.name), " ;")
    isempty(spec.description) ||
        println(plugin, "    rdfs:comment ", _ttl_string(spec.description), " ;")
    # No required feature: the wrapper asks the host for nothing, and a
    # required feature a host lacks is a plugin it refuses to open.
    println(plugin, "    lv2:optionalFeature lv2:hardRTCapable ;")
    println(plugin, "    lv2:port ", join(_lv2_port_block.(lv2_ports(spec)), " , "), " .")
    return (; manifest = String(take!(manifest)), plugin = String(take!(plugin)))
end

# ---------------------------------------------------------------------------
# The format seam
# ---------------------------------------------------------------------------

const LV2_TEMPLATE = normpath(joinpath(@__DIR__, "..", "csrc", "lv2_plugin_template.c"))

function _lv2_substitutions(spec::PluginSpec)
    isempty(spec.pars) && throw(ArgumentError("the parameter struct name is not known yet"))
    lv2_ports(spec)      # the symbols have to be valid before anything is written
    step_args = join(((i.role === :audio ? "x" : "true") for i in spec.inputs), ", ")
    param_info = join(
        (
            "{ $(_c_double(p.min)), $(_c_double(p.max)), $(_c_double(p.default)), " *
                "$(_c_literal(p.stepped)) }," for p in spec.params
        ), "\n    "
    )
    param_apply = join(
        (
            "case $(i - 1): s->pars.$(p.field) = $(_param_cast(p)); break;"
                for (i, p) in enumerate(spec.params)
        ), "\n    "
    )
    constants = join(("s->pars.$k = $(_c_literal(v));" for (k, v) in spec.constants), "\n    ")
    on_rate = spec.sample_rate_field === nothing ? "(void)rate;" :
        "s->pars.$(spec.sample_rate_field) = rate;"
    output_read = spec.sub_clock ?
        "if (o.has_$(spec.output)) s->held[c] = o.$(spec.output);\n" *
        "            double y = s->held[c];" :
        "double y = o.$(spec.output);"
    return Dict(
        "HEADER" => spec.base * ".h",
        "BASE" => spec.base,
        "PARS" => spec.pars,
        "URI" => _c_string(lv2_plugin_uri(spec)),
        "CHANNELS" => string(spec.channels),
        "N_PARAMS" => string(length(spec.params)),
        "LATENCY" => string(spec.latency),
        "PARAM_INFO" => param_info,
        "PARAM_APPLY" => param_apply,
        "ON_RATE" => on_rate,
        "STEP_ARGS" => step_args * ",",
        "OUTPUT_READ" => output_read,
        "CONSTANTS" => constants,
    )
end

"""
    emit_wrapper(::LV2, spec, dir) -> (; sources, include_dirs)

Render `csrc/lv2_plugin_template.c` for `spec` into `dir/lv2_plugin.c`. The
wrapper includes the descriptor's own header and the vendored LV2 headers,
and declares the port layout `AudioPlugins.lv2_ports` describes.
"""
function emit_wrapper(::LV2, spec::PluginSpec, dir::AbstractString)
    subs = _lv2_substitutions(spec)
    spec.step isa CStep && (subs["HEADER"] = basename(spec.step.header))
    path = joinpath(dir, "lv2_plugin.c")
    write(path, _render_template(read(LV2_TEMPLATE, String), subs))
    return (; sources = [path], include_dirs = [VENDOR_DIR])
end

"""
    place_library(::LV2, spec, library, out)

Move the linked shared library at `library` into the LV2 bundle directory at
`out` and write the Turtle beside it: `manifest.ttl`, which is what a host
reads first, and `<stem>.ttl`, which describes the ports. The binary is named
after the bundle, with the platform's extension, because the manifest has to
name it exactly.
"""
function place_library(::LV2, spec::PluginSpec, library::AbstractString, out::AbstractString)
    stem = first(splitext(basename(out)))
    binary = stem * "." * Base.BinaryPlatforms.platform_dlext()
    seealso = stem * ".ttl"
    ttl = lv2_turtle(spec; binary, seealso)     # before moving anything: it can refuse
    mkpath(out)
    mv(library, joinpath(out, binary); force = true)
    write(joinpath(out, "manifest.ttl"), ttl.manifest)
    write(joinpath(out, seealso), ttl.plugin)
    return out
end

const LV2_NO_JULIA_STEP = "export_plugin: LV2 authoring builds a CStep only. A JuliaStep " *
    "brings a Julia runtime, and where that runtime lives inside an .lv2 bundle -- and how " *
    "a host that dlopens the binary from the bundle directory finds it -- is not settled " *
    "here yet. Build the Julia step as a .clap, or give the plugin a C step."

"""
    runtime_layout(::LV2, out)

Refused: LV2 authoring builds a [`CStep`](@ref) only, so no Julia runtime is
ever placed in an `.lv2` bundle.
"""
runtime_layout(::LV2, out::AbstractString) = error(LV2_NO_JULIA_STEP)

_export_julia_step(::LV2, spec::PluginSpec, out::AbstractString; compiler, verbose::Bool) =
    error(LV2_NO_JULIA_STEP)

register_plugin_format!(LV2())
