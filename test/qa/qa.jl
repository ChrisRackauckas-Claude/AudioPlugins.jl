using SciMLTesting, AudioPlugins, Test

# An extension module exists only once its trigger package is loaded, so loading
# JuliaC here is what makes the checks below cover `ext/` and not just `src/`.
using JuliaC

# `using JET` registers JET with SciMLTesting and turns its check on.
using JET

# `AudioPluginsJuliaCExt` uses AudioPlugins' own private helpers, which is what a
# package extension is for. ExplicitImports decides "internal" by `Base.moduleroot`,
# and an extension module's root is itself rather than the package it extends, so
# every such use is reported. These are implementation details of the exporter --
# making them public API would be wrong -- so they are allowed here instead.
const AUDIOPLUGINS_INTERNALS = (
    :VENDOR_DIR, :_export_julia_step, :_resolve_compiler, :_run, :_with_pars,
)

run_qa(
    AudioPlugins;
    ei_kwargs = (;
        all_qualified_accesses_are_public = (;
            ignore = (
                # Base's platform triplet, used to name the platform CLAPHost_jll has
                # no build for. Base declares neither the submodule nor the function
                # `public`, and there is no public equivalent.
                :BinaryPlatforms, :host_triplet,
                # `Base.include(mod, file)` is the documented way to load a file into a
                # module other than the caller's; the module-scoped `include` cannot
                # do it. Documented in the manual, but not declared `public` in Base.
                :include,
                # The JLL interface JLLWrappers generates. Every JLL has these and no
                # JLL declares them `public`.
                :is_available, :libclap_host_path,
                # JuliaC's rpath constants, which any caller of `LinkRecipe` has to
                # name; not declared `public` in JuliaC.
                :RPATH_BUNDLE, :RPATH_JULIA,
                AUDIOPLUGINS_INTERNALS...,
            ),
        ),
        all_explicit_imports_are_public = (; ignore = AUDIOPLUGINS_INTERNALS),
    ),
    # The manual is being added separately (SciML/AudioPlugins.jl has no `docs/`
    # yet), so there is nowhere for a `@docs` block to live. Drop this once it
    # exists; the docstring half of the check is on and must stay green.
    api_docs_kwargs = (; rendered = false),
)
