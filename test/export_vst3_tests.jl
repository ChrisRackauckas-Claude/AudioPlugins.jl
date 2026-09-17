# Authoring VST3: the same hand-written step functions under test/export/
# that export_tests.jl builds as CLAP, built as VST3 bundles here and
# hosted through VST3Host_jll. The expectations are the CLAP suite's,
# against test/export/reference.jl, because a plugin that computes one
# thing as a .clap and another as a .vst3 is a wrapper bug.
#
# The one thing VST3 does differently, and the one thing that can be
# silently wrong, is that parameter values on the wire are normalised to
# [0, 1] while the descriptor is in plain units: every parameter below
# goes through vst3_param_normalized and comes back through the plain
# arithmetic the fixture does.

using Test
using AudioPlugins
using JuliaC      # loaded, so that the JuliaStep refusal below is the format's
using vst3sdk_jll
const AP = AudioPlugins

const FIX = joinpath(@__DIR__, "export")

include(joinpath(FIX, "reference.jl"))

# vst3sdk_jll carries the SDK tree and its static libraries; a checkout of
# the SDK itself is the other thing VST3() takes.
const SDK = VST3(vst3sdk_jll.artifact_dir)

f0, q, gdb = 1000.0, 0.7071067811865476, 6.0
# The host stores samples as float32, so the reference can only be met to
# one float32 ulp of the output; 1e-6 absolute is well inside that and
# would catch a wrong coefficient or a lost state term.
eq_tol = 1.0e-6

@testset "AudioPlugins / export to VST3" begin

    gain_spec = read_plugin_spec(joinpath(FIX, "fx_gain.toml"))
    eq_spec = read_plugin_spec(joinpath(FIX, "fx_eq.toml"))
    decim_spec = read_plugin_spec(joinpath(FIX, "fx_decim.toml"))
    dir = mktempdir()

    @testset "the format registers, and says what it needs" begin
        @test plugin_format("vst3") isa VST3
        @test AP.format_name(SDK) == "vst3"
        @test AP.bundle_extension(SDK) == ".vst3"
        @test isdir(SDK.include_dir) && isdir(SDK.lib_dir)
        # The SDK root may also be the tree itself rather than a JLL artifact.
        @test VST3(SDK.include_dir, SDK.lib_dir).include_dir == SDK.include_dir
        @test_throws ArgumentError VST3("/nonexistent/sdk")
        @test_throws ArgumentError VST3(@__DIR__)             # a directory, but not an SDK
        @test_throws ArgumentError export_plugin(gain_spec, "out.clap"; format = SDK)
        # The format registered as "vst3" names no SDK: it refuses rather
        # than building something that cannot link.
        err = try
            export_plugin(gain_spec, joinpath(dir, "nosdk.vst3"); format = VST3())
            nothing
        catch e
            e
        end
        @test err isa ErrorException && occursin("names no SDK", err.msg)
        @test !ispath(joinpath(dir, "nosdk.vst3"))
    end

    @testset "the class id is derived from the plugin id, and fixed" begin
        # A host remembers the class id, so this hash is part of the ABI:
        # changing it orphans every session that used the old one.
        @test AP._vst3_class_id_string(gain_spec.id) == "9B67DE99431434D141003FC21FF783BF"
        @test AP._vst3_class_id_string(eq_spec.id) != AP._vst3_class_id_string(gain_spec.id)
        @test length(AP._vst3_class_id_string("x")) == 32
        @test all(c -> c in "0123456789ABCDEF", AP._vst3_class_id_string("x"))
    end

    @testset "the rendered wrapper" begin
        mktempdir() do d
            w = AP.emit_wrapper(SDK, gain_spec, d)
            src = read(only(w.sources), String)
            @test !occursin(r"@[A-Z_]+@", src)
            @test occursin("#include \"fx_gain.h\"", src)
            @test occursin("fx_gain_step(x, true, &pars_, &mem_[c])", src)
            @test occursin("case 0: pars_.gain = v; break;", src)
            @test occursin("case 1: pars_.bypass = (v != 0.0); break;", src)
            @test occursin("{ 0u, STR16(\"Gain\"), 0.0, 4.0, 1.0, 0, ParameterInfo::kCanAutomate, false },", src)
            @test occursin("{ 7u, STR16(\"Bypass\"), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate, true },", src)
            @test occursin("SpeakerArr::kStereo", src)
            @test occursin("\"Fx|Tools\"", src)          # from the "utility" feature
            @test occursin("#define AP_LATENCY   0", src)
            @test w.language === :cxx
            @test all(isfile, w.support_sources)
            @test "-lsdk" in w.link_flags
            e = read(only(AP.emit_wrapper(SDK, eq_spec, d).sources), String)
            @test occursin("fx_eq_step(x, &pars_, &mem_[c])", e)
            @test occursin("pars_.fs = sr;", e)
            @test occursin("pars_.enabled = true;", e)
            @test occursin("\"Fx\"", e)                  # no feature with a subcategory
            dec = read(only(AP.emit_wrapper(SDK, decim_spec, d).sources), String)
            @test occursin("if (o.has_y) held_[c] = o.y;", dec)
        end
    end

    gain = export_plugin(gain_spec, joinpath(dir, "fx_gain.vst3"); format = SDK)
    eq = export_plugin(eq_spec, joinpath(dir, "fx_eq.vst3"); format = SDK)
    decim = export_plugin(decim_spec, joinpath(dir, "fx_decim.vst3"); format = SDK)

    @testset "the bundle is laid out the way the SDK's loader reads it" begin
        if Sys.iswindows()
            # A VST3 module on Windows is legally either a bundle directory
            # or a plain DLL named .vst3; this builds the plain DLL.
            @test isfile(gain)
        else
            @test isdir(gain)
            inner = joinpath(gain, "Contents", AP._vst3_module_dir())
            @test isfile(joinpath(inner, Sys.isapple() ? "fx_gain" : "fx_gain.so"))
            if Sys.isapple()
                plist = read(joinpath(gain, "Contents", "Info.plist"), String)
                @test occursin("<key>CFBundleExecutable</key><string>fx_gain</string>", plist)
                @test occursin(gain_spec.id, plist)
            end
        end
        @test !startswith(gain, pkgdir(AudioPlugins))
    end

    @testset "discovery: one class, described by the descriptor" begin
        classes = vst3_scan(gain)
        @test length(classes) == 1
        @test classes[1].id == AP._vst3_class_id_string(gain_spec.id)
        @test classes[1].name == "Fixture Gain"
        @test classes[1].category == "Fx|Tools"
        @test vst3_scan(eq)[1].category == "Fx"
    end

    @testset "parameters: plain in the descriptor, normalised on the wire" begin
        vst3_open!(
            gain; class_id = AP._vst3_class_id_string(gain_spec.id),
            sample_rate = 48000, block_size = 64, channels = 2
        )
        @test vst3_plugin_name() == "Fixture Gain"
        @test vst3_channels() == 2
        @test vst3_latency() == 0.0
        ps = vst3_params()
        @test [p.id for p in ps] == [0.0, 7.0]
        @test ps[1].name == "Gain"
        @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
        @test ps[1].default_normalized == 0.25
        @test ps[1].steps == 0.0
        @test ps[2].name == "Bypass"
        @test (ps[2].min, ps[2].max, ps[2].default) == (0.0, 1.0, 0.0)
        @test ps[2].steps == 1.0                     # stepped, one integer step: a toggle
        # The controller's mapping, which is the one the wrapper applies.
        @test vst3_param_plain(0, 0.5) == 2.0
        @test vst3_param_normalized(0, 2.0) == 0.5
        @test vst3_param_plain(0, 0.0) == 0.0 && vst3_param_plain(0, 1.0) == 4.0
    end

    # The gain fixture is exact in float32 for a power-of-two gain, so these
    # comparisons are equalities and not tolerances.
    function gain_run(x, params...)
        t = vst3_fill!(vec(permutedims([x x])); channels = 2)
        slots = fill(-1.0, 8)
        for (k, (id, plain)) in enumerate(params)
            slots[2k - 1], slots[2k] = id, vst3_param_normalized(id, plain)
        end
        o = AP.vst3_process(t, slots...)
        isnan(o) && error("process failed: $(vst3_last_error())")
        return (vst3_out(o; channel = 0), vst3_out(o; channel = 1))
    end

    @testset "gain at 0.5: y == x * 0.5, sample-exactly, on both channels" begin
        x = signal(64)
        l, r = gain_run(x, (0, 0.5))
        @test l == x .* 0.5
        @test r == x .* 0.5
        @test first(gain_run(x, (0, 1.0))) == x
        @test first(gain_run(x, (0, 2.0))) == x .* 2
    end

    @testset "a stepped parameter rounds, and the range clamps" begin
        x = signal(64)
        @test first(gain_run(x, (0, 0.5), (7, 1.0))) == x            # bypassed
        @test vst3_param_value(7) == 1.0
        # 0.4 plain is below the half-step, so the plugin reads it as 0 --
        # and reports 0 back, rather than the value that arrived.
        @test first(gain_run(x, (0, 0.5), (7, 0.4))) == x .* 0.5
        @test vst3_param_value(7) == 0.0
        # Normalised 1.0 is the top of the plain range, whatever the host sends.
        t = vst3_fill!(vec(permutedims([x x])); channels = 2)
        o = AP.vst3_process(t, 0, 1.0, 7, 0.0, -1, 0, -1, 0)
        @test vst3_out(o) == x .* 4
        @test vst3_param_value(0) == 1.0
    end

    @testset "peaking EQ matches the RBJ recursion at three sample rates" begin
        x = signal(256)
        outs = Vector{Float64}[]
        for fs in (44100, 48000, 96000)
            vst3_open!(eq; sample_rate = fs, block_size = 256, channels = 1)
            @test [p.default for p in vst3_params()] == [f0, q, gdb]
            t = vst3_fill!(x)
            o = AP.vst3_process(
                t, 0, vst3_param_normalized(0, f0), 1, vst3_param_normalized(1, q),
                2, vst3_param_normalized(2, gdb), -1, 0
            )
            y = vst3_out(o)
            @test maximum(abs.(y .- rbj_peaking(x, fs, f0, q, gdb))) < eq_tol
            @test maximum(abs.(y .- x)) > 1.0e-2        # the filter is doing something
            push!(outs, y)
        end
        @test outs[1] != outs[2] && outs[2] != outs[3]
    end

    @testset "2 x 128 frames == 1 x 256 continuous: one state across blocks" begin
        x = signal(256)
        function eq_run(chunk)
            t = vst3_fill!(chunk)
            o = AP.vst3_process(
                t, 0, vst3_param_normalized(0, f0), 1, vst3_param_normalized(1, q),
                2, vst3_param_normalized(2, gdb), -1, 0
            )
            return vst3_out(o)
        end
        vst3_open!(eq; sample_rate = 48000, block_size = 128, channels = 1)
        split = vcat(eq_run(x[1:128]), eq_run(x[129:256]))
        vst3_open!(eq; sample_rate = 48000, block_size = 256, channels = 1)
        whole = eq_run(x)
        @test split == whole
        @test maximum(abs.(whole .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
        # Reopening is a fresh instance, so a run restarted at the boundary
        # must differ: that is what makes the equality above non-vacuous.
        vst3_open!(eq; sample_rate = 48000, block_size = 128, channels = 1)
        a = eq_run(x[1:128])
        vst3_open!(eq; sample_rate = 48000, block_size = 128, channels = 1)
        b = eq_run(x[129:256])
        @test vcat(a, b) != whole
    end

    @testset "a sub-clock output is held between ticks, and keeps its phase" begin
        function decim_run(chunk, gain, divisor)
            t = vst3_fill!(chunk)
            o = AP.vst3_process(
                t, 0, vst3_param_normalized(0, gain), 1, vst3_param_normalized(1, divisor),
                -1, 0, -1, 0
            )
            return vst3_out(o)
        end
        x = signal(96)
        # The divisor is stepped over 1..8: normalised in, an integer out.
        @test vst3_scan(decim)[1].name == "Fixture Decimator"
        vst3_open!(decim; sample_rate = 48000, block_size = 96, channels = 1)
        @test vst3_params()[2].steps == 7.0
        @test vst3_param_plain(1, vst3_param_normalized(1, 3.0)) == 3.0
        @test decim_run(x, 0.5, 3.0) == decimate_hold(x, 3, 0.5)
        vst3_open!(decim; sample_rate = 48000, block_size = 96, channels = 1)
        @test decim_run(x, 1.0, 1.0) == x              # divisor 1: every sample present
        # 32 is not a multiple of 5, so a plugin that restarted its phase at
        # each block would tick at the block boundary and differ.
        vst3_open!(decim; sample_rate = 48000, block_size = 32, channels = 1)
        split = vcat((decim_run(x[(32b + 1):(32b + 32)], 1.0, 5.0) for b in 0:2)...)
        @test split == decimate_hold(x, 5, 1.0)
    end

    @testset "the bus arrangement is the descriptor's, and no wider" begin
        mono = PluginSpec(;
            id = "org.sciml.audioplugins.fixture.vst3mono", name = "Mono Gain",
            base = "fx_gain", pars = "FxGainPars", channels = 1,
            inputs = gain_spec.inputs, source = gain_spec.step.source,
            header = gain_spec.step.header, params = gain_spec.params
        )
        b = export_plugin(mono, joinpath(dir, "mono.vst3"); format = SDK)
        vst3_open!(b; block_size = 32, channels = 1)
        @test vst3_plugin_name() == "Mono Gain"
        @test vst3_channels() == 1
        x = signal(32)
        t = vst3_fill!(x)
        o = AP.vst3_process(t, 0, vst3_param_normalized(0, 0.25), -1, 0, -1, 0, -1, 0)
        @test vst3_out(o) == x .* 0.25
        # One channel is all it declares, so stereo is refused rather than
        # silently given a second channel with nothing behind it.
        @test_throws ErrorException vst3_open!(b; block_size = 32, channels = 2)
        @test occursin("arrangement", vst3_last_error())
    end

    @testset "a declared latency is reported, and the delay is real" begin
        look_spec = read_plugin_spec(joinpath(FIX, "fx_look.toml"))
        @test look_spec.latency == 16
        look = export_plugin(look_spec, joinpath(dir, "fx_look.vst3"); format = SDK)
        vst3_open!(look; sample_rate = 48000, block_size = 64, channels = 1)
        @test vst3_latency() == 16.0            # IAudioProcessor::getLatencySamples()
        x = signal(64)
        t = vst3_fill!(x)
        o = AP.vst3_process(t, 0, vst3_param_normalized(0, 1.0), -1, 0, -1, 0, -1, 0)
        y = vst3_out(o)
        @test all(iszero, y[1:16])
        @test y[17:64] == x[1:48]               # the delay is real, not just declared
        # The wrapper reports the step's latency; it introduces none of its own.
        vst3_open!(gain; sample_rate = 48000, block_size = 64, channels = 1)
        @test vst3_latency() == 0.0
    end

    @testset "a plugin with no parameters at all" begin
        # Nothing on the wire, everything from [constants]: the empty
        # parameter table is its own code path, in C++ as in C.
        fixed = PluginSpec(;
            id = "org.sciml.audioplugins.fixture.vst3fixed", name = "Fixed Gain",
            base = "fx_gain", pars = "FxGainPars", channels = 1,
            inputs = gain_spec.inputs, source = gain_spec.step.source,
            header = gain_spec.step.header, constants = ["gain" => 2.0]
        )
        b = export_plugin(fixed, joinpath(dir, "fixed.vst3"); format = SDK)
        vst3_open!(b; block_size = 32, channels = 1)
        @test vst3_param_count() == 0
        x = signal(32)
        t = vst3_fill!(x)
        @test vst3_out(AP.vst3_process(t, -1, 0, -1, 0, -1, 0, -1, 0)) == x .* 2
    end

    vst3_close!()

    @testset "VST3 authoring builds a C step only, and says so" begin
        jl_spec = read_plugin_spec(joinpath(FIX, "jl_gain.toml"))
        @test jl_spec.step isa JuliaStep
        out = joinpath(dir, "jl_gain.vst3")
        err = try
            export_plugin(jl_spec, out; format = SDK)
            nothing
        catch e
            e
        end
        @test err isa ErrorException && occursin("CStep only", err.msg)
        @test !ispath(out)
        @test_throws ErrorException AP.runtime_layout(VST3(), out)
    end

    # The Julia host has no entry point for IComponent::setState / getState,
    # so the wrapper's preset handling is checked from C++: the rendered
    # wrapper, its step and the SDK linked into one program that calls the
    # factory directly. Same sources export_plugin compiles.
    @testset "presets round-trip through getState / setState" begin
        mktempdir() do d
            w = AP.emit_wrapper(SDK, gain_spec, d)
            cc, cxx = AP._c_compiler(), AP._cxx_compiler()
            @test cxx !== nothing
            step = joinpath(d, "step.o")
            run(`$cc $(AP._c_arch_flags()) -O2 -c $(gain_spec.step.source) -o $step`)
            probe = joinpath(d, "probe_vst3_state")
            memory = joinpath(SDK.include_dir, "public.sdk", "source", "common", "memorystream.cpp")
            run(
                `$cxx $(AP._c_arch_flags()) -std=c++17 -O2 -Wall -Wextra -DRELEASE=1
                 -isystem $(SDK.include_dir) -isystem $FIX -o $probe
                 $(w.sources) $(w.support_sources) $memory
                 $(joinpath(FIX, "probe_vst3_state.cpp")) $step $(w.link_flags)`
            )
            out = read(`$probe`, String)
            @test occursin("ALL PROBES PASS", out)
            @test !occursin("FAIL", out)
        end
    end

end
