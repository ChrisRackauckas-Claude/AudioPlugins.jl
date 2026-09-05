# Authoring plugins: wrap a per-sample C step function in a plugin format
# and build the bundle. The inverse of clap_io.jl, which hosts one.
#
# The interface is deliberately "a C step function plus a descriptor" and
# names no code generator: anything that can emit
#
#     <base>_out <base>_step(<inputs...>, <Pars> *pars, <base>_mem *self);
#     void       <base>_reset(<base>_mem *self);
#
# can be built into a plugin, and the test suite proves the seam with a
# hand-written fixture and no code generator in the loop.

using TOML

export PluginFormat, CLAP, PluginParam, StepInput, PluginSpec, read_plugin_spec,
       export_plugin, register_plugin_format!, plugin_format

# ---------------------------------------------------------------------------
# Formats
# ---------------------------------------------------------------------------

"""
    PluginFormat

Abstract supertype of plugin formats [`export_plugin`](@ref) can build. A
format implements

  * `format_name(fmt) -> String`, the key under which it is registered;
  * `bundle_extension(fmt) -> String`, e.g. `".clap"`;
  * `emit_wrapper(fmt, spec, dir) -> (; sources, include_dirs)`, writing the
    format's wrapper C sources into `dir` — they are compiled with
    `-Wall -Wextra -Werror`;
  * `link_bundle(fmt, spec, out, objects, libs; compiler)`, linking the
    compiled objects into the bundle at `out`.

[`CLAP`](@ref) is the format that ships here. Formats whose SDKs cannot be
vendored in a public repository live out of tree, subtype this, and
[`register_plugin_format!`](@ref) themselves.
"""
abstract type PluginFormat end

"""
    CLAP()

The CLAP plugin format (MIT, header-only; the headers are vendored under
`csrc/vendor/clap`). A bundle is one shared object on Linux and Windows
(`.clap`), and a `Name.clap/Contents/MacOS/Name` directory on macOS.
"""
struct CLAP <: PluginFormat end

format_name(::CLAP) = "clap"
bundle_extension(::CLAP) = ".clap"

const PLUGIN_FORMATS = Dict{String, PluginFormat}()

"""
    register_plugin_format!(fmt::PluginFormat)

Make `fmt` available as `plugin_format(format_name(fmt))`. Registering a
name twice replaces the earlier format.
"""
function register_plugin_format!(fmt::PluginFormat)
    PLUGIN_FORMATS[format_name(fmt)] = fmt
    return fmt
end

"""
    plugin_format(name::AbstractString) -> PluginFormat

The registered format called `name` (`"clap"` ships with the package).
"""
function plugin_format(name::AbstractString)
    haskey(PLUGIN_FORMATS, name) && return PLUGIN_FORMATS[name]
    throw(ArgumentError("no plugin format registered as $(repr(name)); " *
                        "known: $(join(sort!(collect(keys(PLUGIN_FORMATS))), ", "))"))
end

# ---------------------------------------------------------------------------
# The descriptor
# ---------------------------------------------------------------------------

const PARAM_CTYPES = ("double", "bool", "int64_t")

"""
    PluginParam(; id, name, field, min, max, default,
                automatable = true, stepped = false, ctype = "double")

One plugin parameter: the `id` a host addresses it by, its display `name`
and range, and the `field` of the parameter struct it writes. `ctype` is
that field's C type (`"double"`, `"bool"` or `"int64_t"`); a value arriving
from the host is clamped to `[min, max]`, rounded when `stepped`, and
converted.
"""
struct PluginParam
    id::UInt32
    name::String
    field::String
    min::Float64
    max::Float64
    default::Float64
    automatable::Bool
    stepped::Bool
    ctype::String
    function PluginParam(; id, name, field, min, max, default,
                         automatable::Bool = true, stepped::Bool = false,
                         ctype::AbstractString = "double")
        _check_c_ident(field, "parameter field")
        ctype in PARAM_CTYPES ||
            throw(ArgumentError("parameter $(repr(name)): ctype must be one of " *
                                "$(join(PARAM_CTYPES, ", ")), got $(repr(ctype))"))
        all(isfinite, (min, max, default)) ||
            throw(ArgumentError("parameter $(repr(name)): min, max and default must be finite"))
        min <= max ||
            throw(ArgumentError("parameter $(repr(name)): min $min exceeds max $max"))
        min <= default <= max ||
            throw(ArgumentError("parameter $(repr(name)): default $default is outside [$min, $max]"))
        return new(UInt32(id), String(name), String(field), Float64(min), Float64(max),
                   Float64(default), automatable, stepped, String(ctype))
    end
end

"""
    StepInput(name, role)

One argument of the step function, in order. `role` is `:audio` for the
sample in (a `double`) or `:clock` for a "this clock ticked" flag (a `bool`,
always `true`: the wrapper calls the step once per sample).
"""
struct StepInput
    name::String
    role::Symbol
    function StepInput(name, role)
        role in (:audio, :clock) ||
            throw(ArgumentError("step input $(repr(name)): role must be :audio or :clock, got $(repr(role))"))
        return new(String(name), role)
    end
end

"""
    PluginSpec(; id, name, base, pars, source, header, kwargs...)

Everything [`export_plugin`](@ref) needs, and nothing about where the C
came from. Required:

  * `id`, `name` — the plugin's identifier (reverse-DNS by convention) and display name;
  * `base` — the ABI base name: the step function is `<base>_step`, its
    state `<base>_mem`, its result `<base>_out`, and `<base>_reset` clears the state;
  * `pars` — the name of the parameter struct type declared in `header`;
  * `source`, `header` — the C file defining the ABI and the header declaring it.

Optional:

  * `vendor`, `version`, `description`, `url` — descriptor strings;
  * `features` — CLAP feature strings, default `["audio-effect"]`;
  * `channels` — channels per port, default `2`; one `<base>_mem` per channel;
  * `inputs` — the step arguments before `pars`, as [`StepInput`](@ref)s;
    default one `:audio` input. Exactly one must be `:audio`;
  * `output` — the field of `<base>_out` carrying the sample, default `"y"`.
    Outputs on a sub-clock (carrying a `has_<name>` flag) are not supported;
  * `sample_rate_field` — a field of the parameter struct to write the
    host's sample rate into on activate, so one bundle serves every rate;
  * `params` — [`PluginParam`](@ref)s;
  * `constants` — `field => value` pairs written into the parameter struct
    once at instantiation, for fields that are not plugin parameters;
  * `pkgconfig` — the `.pc` file describing how to compile and link
    `source`; its `Cflags` and `Libs` go on the command lines. Use it
    rather than hardcoding `-lm`;
  * `include_dirs` — extra include directories.

[`read_plugin_spec`](@ref) builds one from a TOML file.
"""
struct PluginSpec
    id::String
    name::String
    vendor::String
    version::String
    description::String
    url::String
    features::Vector{String}
    channels::Int
    base::String
    pars::String
    inputs::Vector{StepInput}
    output::String
    sample_rate_field::Union{Nothing, String}
    params::Vector{PluginParam}
    constants::Vector{Pair{String, Any}}
    source::String
    header::String
    pkgconfig::Union{Nothing, String}
    include_dirs::Vector{String}
end

function PluginSpec(; id, name, base, pars, source, header,
                    vendor = "", version = "0.0.0", description = "", url = "",
                    features = ["audio-effect"], channels::Integer = 2,
                    inputs = [StepInput("u", :audio)], output = "y",
                    sample_rate_field = nothing, params = PluginParam[],
                    constants = Pair{String, Any}[], pkgconfig = nothing,
                    include_dirs = String[])
    isempty(id) && throw(ArgumentError("plugin id must not be empty"))
    isempty(name) && throw(ArgumentError("plugin name must not be empty"))
    1 <= channels <= 64 || throw(ArgumentError("channels must be in 1..64, got $channels"))
    _check_c_ident(base, "ABI base name")
    _check_c_ident(pars, "parameter struct name")
    _check_c_ident(output, "output field")
    sample_rate_field === nothing || _check_c_ident(sample_rate_field, "sample_rate_field")
    inputs = StepInput[i isa StepInput ? i : StepInput(i...) for i in inputs]
    count(i -> i.role === :audio, inputs) == 1 ||
        throw(ArgumentError("the step function must take exactly one :audio input"))
    params = PluginParam[params...]
    ids = [p.id for p in params]
    allunique(ids) || throw(ArgumentError("parameter ids must be unique, got $ids"))
    consts = Pair{String, Any}[String(k) => v for (k, v) in constants]
    for (k, v) in consts
        _check_c_ident(k, "constant field")
        v isa Union{Bool, Integer, AbstractFloat} ||
            throw(ArgumentError("constant $k must be a Bool, Integer or Float, got $(typeof(v))"))
    end
    for (label, path) in (("source", source), ("header", header), ("pkgconfig", pkgconfig))
        path === nothing && continue
        isfile(path) || throw(ArgumentError("$label $(repr(path)) is not a file"))
    end
    return PluginSpec(String(id), String(name), String(vendor), String(version),
                      String(description), String(url), String[features...], Int(channels),
                      String(base), String(pars), inputs, String(output),
                      sample_rate_field === nothing ? nothing : String(sample_rate_field),
                      params, consts, abspath(source), abspath(header),
                      pkgconfig === nothing ? nothing : abspath(pkgconfig),
                      String[abspath(d) for d in include_dirs])
end

function _check_c_ident(s, what)
    occursin(r"^[A-Za-z_][A-Za-z0-9_]*$", s) ||
        throw(ArgumentError("$what must be a C identifier, got $(repr(s))"))
    return nothing
end

"""
    read_plugin_spec(path) -> PluginSpec

Read a descriptor from a TOML file. Relative paths in `[build]` resolve
against the file's directory. The layout, with every optional key shown:

```toml
schema = 1

[plugin]
id = "org.example.gain"
name = "Example Gain"
vendor = "Example"            # optional, as are version, description, url
version = "0.1.0"
features = ["audio-effect"]   # optional
channels = 2                  # optional

[abi]
base = "ex_gain"              # ex_gain_step, ex_gain_reset, ex_gain_mem, ex_gain_out
pars = "ExGainPars"           # the parameter struct declared in the header
inputs = [{ name = "u", role = "audio" }, { name = "clock", role = "clock" }]
output = "y"                  # field of ex_gain_out; optional, default "y"
sample_rate_field = "fs"      # optional

[build]
source = "ex_gain.c"
header = "ex_gain.h"
pkgconfig = "ex_gain.pc"      # optional
include_dirs = []             # optional

[[param]]
id = 0
name = "Gain"
field = "gain"
min = 0.0
max = 4.0
default = 1.0
automatable = true            # optional
stepped = false               # optional
ctype = "double"              # optional: double, bool or int64_t

[constants]                   # optional: struct fields fixed at instantiation
enabled = true
```

The descriptor format is versioned by `schema`; only `1` exists.
"""
function read_plugin_spec(path::AbstractString)
    d = TOML.parsefile(path)
    dir = dirname(abspath(path))
    get(d, "schema", 1) == 1 ||
        throw(ArgumentError("$path: unsupported descriptor schema $(d["schema"])"))
    plugin = _section(d, "plugin", path)
    abi = _section(d, "abi", path)
    build = _section(d, "build", path)
    resolve(p) = p === nothing ? nothing : normpath(joinpath(dir, p))
    inputs = [StepInput(i["name"], Symbol(i["role"])) for i in get(abi, "inputs", Any[])]
    params = [PluginParam(; id = p["id"], name = p["name"], field = p["field"],
                          min = p["min"], max = p["max"], default = p["default"],
                          automatable = get(p, "automatable", true),
                          stepped = get(p, "stepped", false),
                          ctype = get(p, "ctype", "double"))
              for p in get(d, "param", Any[])]
    return PluginSpec(;
        id = plugin["id"], name = plugin["name"],
        vendor = get(plugin, "vendor", ""), version = get(plugin, "version", "0.0.0"),
        description = get(plugin, "description", ""), url = get(plugin, "url", ""),
        features = get(plugin, "features", ["audio-effect"]),
        channels = get(plugin, "channels", 2),
        base = abi["base"], pars = abi["pars"],
        inputs = isempty(inputs) ? [StepInput("u", :audio)] : inputs,
        output = get(abi, "output", "y"),
        sample_rate_field = get(abi, "sample_rate_field", nothing),
        params, constants = collect(get(d, "constants", Dict{String, Any}())),
        source = resolve(build["source"]), header = resolve(build["header"]),
        pkgconfig = resolve(get(build, "pkgconfig", nothing)),
        include_dirs = [resolve(i) for i in get(build, "include_dirs", String[])])
end

function _section(d, key, path)
    haskey(d, key) || throw(ArgumentError("$path: missing [$key] table"))
    return d[key]
end

# ---------------------------------------------------------------------------
# pkg-config files, read without pkg-config
# ---------------------------------------------------------------------------

"""
    pkgconfig_flags(path) -> (; cflags::Vector{String}, libs::Vector{String})

The `Cflags` and `Libs` (plus `Libs.private`, since the object is linked
directly) of a `.pc` file, with `\${var}` references expanded from the
file's own definitions. Reading the file directly avoids requiring the
`pkg-config` binary, which a machine with a C compiler need not have.
"""
function pkgconfig_flags(path::AbstractString)
    vars = Dict{String, String}()
    fields = Dict{String, String}()
    for raw in eachline(path)
        line = strip(first(split(raw, '#'; limit = 2)))
        isempty(line) && continue
        m = match(r"^([A-Za-z0-9_.]+)\s*(=|:)\s*(.*)$", line)
        m === nothing && continue
        name, sep, value = m.captures
        if sep == "="
            vars[name] = _expand_pc(value, vars)
        else
            fields[name] = _expand_pc(value, vars)
        end
    end
    libs = vcat(Base.shell_split(get(fields, "Libs", "")),
                Base.shell_split(get(fields, "Libs.private", "")))
    return (; cflags = Base.shell_split(get(fields, "Cflags", "")), libs)
end

_expand_pc(s, vars) = replace(s, r"\$\{([A-Za-z0-9_.]+)\}" => m -> get(vars, m[3:(end - 1)], ""))

# ---------------------------------------------------------------------------
# Rendering C
# ---------------------------------------------------------------------------

const CLAP_TEMPLATE = normpath(joinpath(@__DIR__, "..", "csrc", "clap_plugin_template.c"))
const VENDOR_DIR = normpath(joinpath(@__DIR__, "..", "csrc", "vendor"))

function _c_string(s::AbstractString)
    io = IOBuffer()
    print(io, '"')
    for c in s
        if c == '"' || c == '\\'
            print(io, '\\', c)
        elseif c == '\n'
            print(io, "\\n")
        elseif isascii(c) && !isprint(c)
            throw(ArgumentError("control character in string $(repr(s))"))
        else
            print(io, c)
        end
    end
    print(io, '"')
    return String(take!(io))
end

function _c_double(x::Real)
    isfinite(x) || throw(ArgumentError("cannot emit non-finite $x as a C double"))
    return repr(Float64(x))
end

_c_literal(v::Bool) = v ? "true" : "false"
_c_literal(v::Integer) = string(v)
_c_literal(v::AbstractFloat) = _c_double(v)

function _param_cast(p::PluginParam)
    p.ctype == "double" && return "v"
    p.ctype == "bool" && return "(v != 0.0)"
    return "(int64_t)v"
end

function _param_flags(p::PluginParam)
    flags = String[]
    p.automatable && push!(flags, "CLAP_PARAM_IS_AUTOMATABLE")
    p.stepped && push!(flags, "CLAP_PARAM_IS_STEPPED")
    return isempty(flags) ? "0u" : join(flags, " | ")
end

function _render_template(template::AbstractString, subs::Dict{String, String})
    out = template
    for (k, v) in subs
        out = replace(out, "@$k@" => v)
    end
    left = unique(eachmatch(r"@[A-Z_]+@", out))
    isempty(left) || error("unrendered tokens in template: $(join((m.match for m in left), ", "))")
    return out
end

function _clap_substitutions(spec::PluginSpec)
    step_args = join(((i.role === :audio ? "x" : "true") for i in spec.inputs), ", ")
    port_type = spec.channels == 1 ? "CLAP_PORT_MONO" :
                spec.channels == 2 ? "CLAP_PORT_STEREO" : "NULL"
    param_info = join(("{ $(p.id)u, $(_c_string(p.name)), $(_c_double(p.min)), " *
                       "$(_c_double(p.max)), $(_c_double(p.default)), $(_param_flags(p)) },"
                       for p in spec.params), "\n    ")
    param_apply = join(("case $(i - 1): s->pars.$(p.field) = $(_param_cast(p)); break;"
                        for (i, p) in enumerate(spec.params)), "\n    ")
    constants = join(("s->pars.$k = $(_c_literal(v));" for (k, v) in spec.constants), "\n    ")
    on_activate = spec.sample_rate_field === nothing ? "(void)sr;" :
                  "s->pars.$(spec.sample_rate_field) = sr;"
    return Dict(
        "HEADER" => basename(spec.header),
        "BASE" => spec.base,
        "PARS" => spec.pars,
        "CHANNELS" => string(spec.channels),
        "N_PARAMS" => string(length(spec.params)),
        "FEATURES" => join((_c_string(f) * "," for f in spec.features), " "),
        "ID" => _c_string(spec.id),
        "NAME" => _c_string(spec.name),
        "VENDOR" => _c_string(spec.vendor),
        "URL" => _c_string(spec.url),
        "VERSION" => _c_string(spec.version),
        "DESCRIPTION" => _c_string(spec.description),
        "PARAM_INFO" => param_info,
        "PARAM_APPLY" => param_apply,
        "PORT_TYPE" => port_type,
        "ON_ACTIVATE" => on_activate,
        "STEP_ARGS" => step_args * ",",
        "OUTPUT" => spec.output,
        "CONSTANTS" => constants,
    )
end

"""
    emit_wrapper(::CLAP, spec, dir) -> (; sources, include_dirs)

Render `csrc/clap_plugin_template.c` for `spec` into `dir/clap_plugin.c`.
"""
function emit_wrapper(::CLAP, spec::PluginSpec, dir::AbstractString)
    src = _render_template(read(CLAP_TEMPLATE, String), _clap_substitutions(spec))
    path = joinpath(dir, "clap_plugin.c")
    write(path, src)
    return (; sources = [path], include_dirs = [VENDOR_DIR])
end

# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

function _resolve_compiler(compiler)
    compiler === nothing || return String(compiler)
    cc = _c_compiler()
    cc === nothing && error("export_plugin needs a C compiler on PATH (tried cc, gcc, clang), " *
                            "or one passed as `compiler`. Hosting plugins does not.")
    return cc
end

function _run(cmd::Cmd, verbose::Bool)
    verbose && println(stderr, "[export_plugin] ", cmd)
    run(cmd)
    return nothing
end

"""
    link_bundle(::CLAP, spec, out, objects, libs; compiler, verbose = false)

Link `objects` into the CLAP bundle at `out`: a shared object on Linux and
Windows, a `Contents/MacOS` directory bundle with an `Info.plist` on macOS.
"""
function link_bundle(::CLAP, spec::PluginSpec, out::AbstractString, objects, libs;
                     compiler, verbose::Bool = false)
    if Sys.isapple()
        stem = first(splitext(basename(out)))
        macos = joinpath(out, "Contents", "MacOS")
        mkpath(macos)
        write(joinpath(out, "Contents", "Info.plist"), _info_plist(spec, stem))
        write(joinpath(out, "Contents", "PkgInfo"), "BNDL????")
        binary = joinpath(macos, stem)
        _run(`$compiler -shared -o $binary $objects $libs`, verbose)
    else
        mkpath(dirname(out))
        _run(`$compiler -shared -fPIC -o $out $objects $libs -Wl,--no-undefined`, verbose)
    end
    return out
end

function _info_plist(spec::PluginSpec, stem::AbstractString)
    esc(s) = replace(s, "&" => "&amp;", "<" => "&lt;", ">" => "&gt;")
    return """
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0">
    <dict>
        <key>CFBundleDevelopmentRegion</key><string>English</string>
        <key>CFBundleExecutable</key><string>$(esc(stem))</string>
        <key>CFBundleIdentifier</key><string>$(esc(spec.id))</string>
        <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
        <key>CFBundleName</key><string>$(esc(spec.name))</string>
        <key>CFBundlePackageType</key><string>BNDL</string>
        <key>CFBundleShortVersionString</key><string>$(esc(spec.version))</string>
        <key>CFBundleVersion</key><string>$(esc(spec.version))</string>
    </dict>
    </plist>
    """
end

"""
    export_plugin(spec::PluginSpec, out; format = CLAP(), compiler = nothing,
                  verbose = false) -> out

Build the plugin described by `spec` into the bundle at `out`, whose
extension must be the format's (`.clap`). The wrapper is rendered and
compiled under `-Wall -Wextra -Werror`; `spec.source` is compiled without
those, since generated C tends to carry harmless unused temporaries; both
are built with hidden visibility so that two plugins sharing an ABI base
name can coexist in one host process. Link flags come from the
descriptor's `.pc` file; an undefined symbol fails the link rather than
the first `dlopen`.

This is the one operation in the package that needs a C compiler: `cc`,
`gcc` or `clang` on `PATH`, or `compiler = "/path/to/cc"`.
"""
function export_plugin(spec::PluginSpec, out::AbstractString; format::PluginFormat = CLAP(),
                       compiler = nothing, verbose::Bool = false)
    ext = bundle_extension(format)
    endswith(out, ext) ||
        throw(ArgumentError("output $(repr(out)) must end in $ext for format $(format_name(format))"))
    out = abspath(out)
    cc = _resolve_compiler(compiler)
    pc = spec.pkgconfig === nothing ? (; cflags = String[], libs = String[]) :
         pkgconfig_flags(spec.pkgconfig)
    mktempdir() do dir
        wrapper = emit_wrapper(format, spec, dir)
        # -isystem rather than -I for the wrapper: the ABI header is someone
        # else's code and must not fail our -Werror build.
        incs = String[]
        for d in [wrapper.include_dirs; dirname(spec.header); dirname(spec.source); spec.include_dirs]
            push!(incs, "-isystem", d)
        end
        objects = String[]
        strict = ["-std=gnu99", "-Wall", "-Wextra", "-Werror"]
        common = ["-O2", "-fPIC", "-fvisibility=hidden", incs..., pc.cflags...]
        for (i, src) in enumerate(wrapper.sources)
            obj = joinpath(dir, "wrapper_$i.o")
            _run(`$cc $strict $common -c $src -o $obj`, verbose)
            push!(objects, obj)
        end
        model = joinpath(dir, "model.o")
        _run(`$cc $common -c $(spec.source) -o $model`, verbose)
        push!(objects, model)
        link_bundle(format, spec, out, objects, pc.libs; compiler = cc, verbose)
    end
    return out
end

register_plugin_format!(CLAP())

@static if VERSION >= v"1.11"
    eval(Meta.parse("public format_name, bundle_extension, emit_wrapper, link_bundle, pkgconfig_flags"))
end
