# The bundle registry: which .clap modules are available, and what is in them.
#
# Nothing here downloads, ships or enumerates anything on its own. A plugin
# collection is a JLL, the JLL is wrapped by a sublibrary package under lib/,
# and that sublibrary calls `register_bundle!` with the path the JLL exposes.
# Install no sublibrary and the registry is empty; install one and its plugins
# are openable by id. That is the whole mechanism -- deliberately, because the
# collections worth bundling carry licences (GPL, mostly) that must not become
# a transitive obligation of an MIT package that merely knows how to host them.
#
# A sublibrary is therefore about six lines:
#
#     module AudioPluginsStandardFX
#     using AudioPlugins: register_bundle!
#     using StandardFX_jll: StandardFX_jll
#     __init__() = register_bundle!(StandardFX_jll.standardfx_clap; source = StandardFX_jll)
#     end
#
# A package that already depends on the JLL for its own reasons can register
# from a package extension instead; the registry does not care which.
#
# The registry is a *cache of descriptors*, not of open plugins: registering
# scans and records, and nothing is instantiated until `clap_open!`.

export register_bundle!, unregister_bundle!, bundles, plugins

"""
    AudioPlugins.RegisteredBundle

One registered `.clap` module: where it is, what registered it, and the
descriptors [`register_bundle!`](@ref) read out of it.
"""
struct RegisteredBundle
    path::String
    source::Union{Module, Nothing}
    entries::Vector{@NamedTuple{id::String, name::String}}
end

# Ordered rather than a Dict so that `plugins()` lists bundles in the order
# they were registered, which is the order the user loaded the JLLs in. A
# listing that permuted itself between sessions would be useless for eyeballing
# a 500-plugin collection.
const BUNDLE_REGISTRY = RegisteredBundle[]
const BUNDLE_LOCK = ReentrantLock()

"""
    register_bundle!(path; source = nothing) -> Int

Scan the `.clap` module at `path` and record every plugin it declares, so that
[`plugins`](@ref) lists them and [`clap_open!`](@ref) can open one by its id
alone. Returns how many plugins were registered.

`source` is the module that owns the bundle -- a JLL, for a plugin collection
shipped as one -- and is what [`plugins`](@ref) filters on when asked for a
single collection's plugins. It is a label; the registry never loads it.

Registering the same path again rescans it and replaces what was recorded,
so a bundle that changed on disk can be picked up without restarting.

!!! warning
    Scanning **closes whatever plugin is open**, because the host holds one
    module at a time and a scan has to load the one being scanned. Register
    bundles before opening a plugin, not between blocks.

Throws when the bundle cannot be loaded, with the host's own message -- a
collection whose descriptors disagree with reality is then caught here, at
registration, rather than in the middle of a render.
"""
function register_bundle!(path::AbstractString; source::Union{Module, Nothing} = nothing)
    p = abspath(String(path))
    entries = clap_scan(p)          # throws with the host's message on failure
    return @lock BUNDLE_LOCK begin
        i = findfirst(b -> b.path == p, BUNDLE_REGISTRY)
        b = RegisteredBundle(p, source, entries)
        i === nothing ? push!(BUNDLE_REGISTRY, b) : (BUNDLE_REGISTRY[i] = b)
        length(entries)
    end
end

"""
    unregister_bundle!(path) -> Bool

Forget the bundle at `path`, returning whether it was registered. Does not
touch an open plugin: a plugin already instantiated from that bundle goes on
working, because what is being dropped is the descriptor record, not the
module.
"""
function unregister_bundle!(path::AbstractString)
    p = abspath(String(path))
    return @lock BUNDLE_LOCK begin
        i = findfirst(b -> b.path == p, BUNDLE_REGISTRY)
        i === nothing ? false : (deleteat!(BUNDLE_REGISTRY, i); true)
    end
end

"""
    bundles() -> Vector{@NamedTuple{path::String, source, n::Int}}

Every registered bundle, in registration order: where it is, the module that
registered it (`nothing` when it was registered by hand) and how many plugins
it declares.
"""
bundles() = @lock BUNDLE_LOCK [
    (path = b.path, source = b.source, n = length(b.entries)) for b in BUNDLE_REGISTRY
]

"""
    plugins() -> Vector{@NamedTuple{id, name, bundle, index, source}}
    plugins(bundle::AbstractString)
    plugins(source::Module)

Every plugin the registry knows, or just those of one bundle (by path) or one
collection (by the module that registered it). `index` is the plugin's position
in its own bundle's factory.

Empty until something is registered: no plugin collection ships with this
package, and loading none leaves the registry empty rather than reaching for a
default. `id` is what [`clap_open!`](@ref) takes.
"""
plugins() = @lock BUNDLE_LOCK _entries(BUNDLE_REGISTRY)

plugins(bundle::AbstractString) =
    (p = abspath(String(bundle)); @lock BUNDLE_LOCK _entries(b -> b.path == p))

plugins(source::Module) = @lock BUNDLE_LOCK _entries(b -> b.source === source)

_entries(pred::Function) = _entries(filter(pred, BUNDLE_REGISTRY))

_entries(bs::AbstractVector{RegisteredBundle}) = [
    (id = e.id, name = e.name, bundle = b.path, index = i - 1, source = b.source)
        for b in bs for (i, e) in enumerate(b.entries)
]

"""
    AudioPlugins.find_plugin(id) -> @NamedTuple{id, name, bundle, index, source}

The registered plugin with this id, or `nothing` when no bundle declares it.
Throws when more than one does: two collections that both claim an id is a
packaging mistake, and picking one silently would make which effect you got
depend on JLL load order.
"""
function find_plugin(id::AbstractString)
    matches = filter(e -> e.id == id, plugins())
    isempty(matches) && return nothing
    length(matches) == 1 && return only(matches)
    return error(
        "plugin id $(repr(id)) is declared by $(length(matches)) registered bundles " *
            "($(join((repr(m.bundle) for m in matches), ", "))); open it by path with " *
            "clap_open!(bundle; plugin_id = $(repr(id))) to say which one you mean"
    )
end
