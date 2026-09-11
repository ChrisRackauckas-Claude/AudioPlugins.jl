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

## LV2 hosting

The LV2 counterpart of the CLAP surface above, function for function. A plugin is named by
a URI and found on a search path of bundle directories rather than in one bundle file, and
a parameter is a control input port whose `id` is its port index.

```@docs
lv2_host_available
lv2_lib_path
lv2_src_path
lv2_test_bundle
lv2_scan
lv2_open!
lv2_close!
lv2_is_open
lv2_plugin_name
lv2_plugin_uri
lv2_last_error
lv2_block_size
lv2_sample_rate
lv2_channels
lv2_n_audio_in
lv2_n_audio_out
lv2_latency
lv2_n_process
lv2_reset_counters!
lv2_params
lv2_param_count
AudioPlugins.lv2_param_value
lv2_fill!
lv2_out
```

### LV2 node-side operators

```@docs
AudioPlugins.lv2p_in_tone
AudioPlugins.lv2p_process
AudioPlugins.lv2p_in_sample
AudioPlugins.lv2p_out_sample
AudioPlugins.lv2p_out_count
AudioPlugins.lv2p_out_rms
AudioPlugins.lv2p_out_peak
AudioPlugins.lv2p_out_valid
```

### LV2 waveform codes

```@docs
LV2_WAVE_SILENCE
LV2_WAVE_SINE
LV2_WAVE_SQUARE
LV2_WAVE_RAMP
LV2_WAVE_IMPULSE
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
