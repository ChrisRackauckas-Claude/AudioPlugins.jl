# API

```@meta
CurrentModule = AudioPlugins
```

Every exported name is listed here. Names shown as `AudioPlugins.something` are not
exported: they are the format-extension interface and the node-side operators, documented
because writing a new format or driving the processing path needs them.

## The module

```@docs
AudioPlugins
```

## Hosting: the library

```@docs
clap_host_available
clap_lib_path
clap_src_path
build_clap_host!
clap_test_bundle
```

## Hosting: lifecycle and discovery

```@docs
clap_scan
clap_open!
clap_close!
clap_is_open
clap_plugin_name
clap_last_error
```

## Hosting: the bundle registry

Which `.clap` modules are available, and what is in them. Nothing is registered until
something registers it — see [Plugin collections: the bundle registry](@ref).

```@docs
register_bundle!
unregister_bundle!
bundles
plugins
AudioPlugins.find_plugin
AudioPlugins.RegisteredBundle
```

## Hosting: configuration in force

```@docs
clap_block_size
clap_sample_rate
clap_latency
clap_n_process
clap_reset_counters!
```

## Hosting: parameters

```@docs
clap_params
clap_param_count
AudioPlugins.clap_param_value
```

## Hosting: audio in and out

```@docs
clap_fill!
clap_out
```

## Node-side operators

One equation, one call. Each takes the token of the block it depends on, so nothing can be
scheduled before the block it reads. Not exported: a Julia caller reaches them as
`AudioPlugins.clp_process` and so on.

```@docs
AudioPlugins.clp_in_tone
AudioPlugins.clp_process
AudioPlugins.clp_in_sample
AudioPlugins.clp_out_sample
AudioPlugins.clp_out_rms
AudioPlugins.clp_out_peak
AudioPlugins.clp_out_valid
```

### Waveform codes

```@docs
CLAP_WAVE_SILENCE
CLAP_WAVE_SINE
CLAP_WAVE_SQUARE
CLAP_WAVE_RAMP
CLAP_WAVE_IMPULSE
```

## Authoring: the descriptor

```@docs
PluginSpec
read_plugin_spec
PluginParam
StepInput
```

## Authoring: step sources

```@docs
StepSource
CStep
JuliaStep
```

## Authoring: building

```@docs
export_plugin
```

## Authoring: formats

```@docs
PluginFormat
CLAP
register_plugin_format!
plugin_format
```

The methods a [`PluginFormat`](@ref) implements, with CLAP's as the worked example:

```@docs
AudioPlugins.format_name
AudioPlugins.bundle_extension
AudioPlugins.emit_wrapper
AudioPlugins.place_library
AudioPlugins.runtime_layout
```

## Authoring: helpers

```@docs
AudioPlugins.pkgconfig_flags
AudioPlugins.julia_step_header
```
