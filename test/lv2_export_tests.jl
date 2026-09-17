# LV2 authoring: the bundles built here come from the same hand-written step
# functions under test/export/ that the CLAP authoring suite uses, and are
# then discovered and hosted through lilv by the LV2 host of this package.
#
# An LV2 plugin's metadata is not in its binary: a host knows only what the
# Turtle beside it says. So every declaration the exporter writes is checked
# by reading it back out of lilv -- port classes, indices, symbols, names,
# ranges and the designated latency port -- and not by trusting the text.

using Test
using AudioPlugins
const AP = AudioPlugins

const FIX = joinpath(@__DIR__, "export")

# Inputs that are exactly representable as float32, so that an exact
# comparison against the host's float32 buffers means what it says.
f32(x) = Float64.(Float32.(x))
signal(n) = f32([0.4sin(2pi * 0.013i) + 0.3sin(2pi * 0.171i) for i in 0:(n - 1)])

# The reference recursion, written the way fx_eq.c writes it so the only
# differences are the host's float32 samples and LV2's float32 control ports.
function rbj_peaking(x, fs, f0, q, gain_db)
    A = 10.0^(gain_db / 40)
    w0 = 2pi * f0 / fs
    alpha = sin(w0) / (2q)
    cw = cos(w0)
    b0, b1, b2 = 1 + alpha * A, -2cw, 1 - alpha * A
    a0, a1, a2 = 1 + alpha / A, -2cw, 1 - alpha / A
    x1 = x2 = y1 = y2 = 0.0
    y = similar(x)
    for i in eachindex(x)
        y[i] = (b0 / a0) * x[i] + (b1 / a0) * x1 + (b2 / a0) * x2 - (a1 / a0) * y1 - (a2 / a0) * y2
        x2, x1 = x1, x[i]
        y2, y1 = y1, y[i]
    end
    return y
end

# A held sample-and-hold: y[i] = gain * x[k] for the latest k <= i with k % d == 0.
function decimate_hold(x, d, gain)
    y = similar(x)
    held = 0.0
    for (i, v) in enumerate(x)
        (i - 1) % d == 0 && (held = gain * v)
        y[i] = held
    end
    return y
end

# One block in, one block out, with up to four control ports set by index.
function process(x, params...)
    t = lv2_fill!(x)
    slots = fill(-1.0, 8)
    for (k, (id, v)) in enumerate(params)
        slots[2k - 1], slots[2k] = id, v
    end
    o = AP.lv2_process(t, slots...)
    isnan(o) && error("lv2_process failed: $(lv2_last_error())")
    return lv2_out(o)
end

spec_of(name) = read_plugin_spec(joinpath(FIX, name))
# Descriptor values as a control port carries them: LV2 control ports are
# floats, so a range or a default only survives to float precision.
as_float(x) = Float64(Float32(x))

@testset "AudioPlugins / LV2 export" begin

    gain_spec = spec_of("fx_gain.toml")
    eq_spec = spec_of("fx_eq.toml")
    look_spec = spec_of("fx_look.toml")
    decim_spec = spec_of("fx_decim.toml")

    dir = mktempdir()
    build(spec, stem) = export_plugin(spec, joinpath(dir, stem * ".lv2"); format = LV2())
    gain = build(gain_spec, "fx_gain")
    eq = build(eq_spec, "fx_eq")
    look = build(look_spec, "fx_look")
    decim = build(decim_spec, "fx_decim")

    dlext = Base.BinaryPlatforms.platform_dlext()
    LV2_PATH = lv2_default_path(dir)

    @testset "the format is registered and names its bundles" begin
        @test plugin_format("lv2") === LV2()
        @test AP.format_name(LV2()) == "lv2"
        @test AP.bundle_extension(LV2()) == ".lv2"
        @test_throws ArgumentError export_plugin(gain_spec, joinpath(dir, "x.clap"); format = LV2())
    end

    @testset "the plugin URI is derived from the descriptor id" begin
        uri(id) = AP.lv2_plugin_uri(
            PluginSpec(;
                id, name = "N", base = "fx_gain", pars = "FxGainPars",
                source = gain_spec.step.source, header = gain_spec.step.header,
                inputs = gain_spec.inputs
            )
        )
        @test uri("org.example.gain") == "urn:audioplugins:org.example.gain"
        # An id that is already a URI is the URI, so an author can publish
        # under a domain they control without a second identifier.
        @test uri("http://example.org/plugins/gain") == "http://example.org/plugins/gain"
        @test uri("urn:example:gain") == "urn:example:gain"
        @test_throws ArgumentError uri("not a name")
        @test_throws ArgumentError uri("http://example.org/a b")
        @test AP.lv2_plugin_uri(gain_spec) == "urn:audioplugins:$(gain_spec.id)"
    end

    @testset "the bundle is a directory of binary and Turtle" begin
        @test isdir(gain)
        @test isfile(joinpath(gain, "fx_gain." * dlext))
        @test isfile(joinpath(gain, "manifest.ttl"))
        @test isfile(joinpath(gain, "fx_gain.ttl"))
        @test !startswith(gain, pkgdir(AudioPlugins))
        manifest = read(joinpath(gain, "manifest.ttl"), String)
        @test occursin("<urn:audioplugins:$(gain_spec.id)>", manifest)
        @test occursin("lv2:binary <fx_gain.$dlext> ;", manifest)
        @test occursin("rdfs:seeAlso <fx_gain.ttl> .", manifest)
    end

    @testset "the generated Turtle says what the descriptor says" begin
        ttl = read(joinpath(gain, "fx_gain.ttl"), String)
        @test occursin("doap:name \"Fixture Gain\" ;", ttl)
        @test occursin("lv2:index 0 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;", ttl)
        @test occursin("lv2:minimum 0.0 ; lv2:maximum 4.0 ; lv2:default 1.0", ttl)
        @test occursin("lv2:index 2 ; lv2:symbol \"in_0\" ; lv2:name \"In 1\"", ttl)
        @test occursin("lv2:index 4 ; lv2:symbol \"out_0\" ; lv2:name \"Out 1\"", ttl)
        # Nothing is required of the host, so nothing can make it refuse to open.
        @test !occursin("lv2:requiredFeature", ttl)
        @test occursin("lv2:optionalFeature lv2:hardRTCapable ;", ttl)
        look_ttl = read(joinpath(look, "fx_look.ttl"), String)
        @test occursin(
            "lv2:designation lv2:latency ; lv2:portProperty lv2:reportsLatency", look_ttl
        )
        @test !occursin("lv2:designation", ttl)      # and only where there is a latency

        # The port layout the Turtle publishes is the one the C wrapper is
        # compiled against: parameters, then audio in, then audio out.
        ports = AP.lv2_ports(gain_spec)
        @test [p.index for p in ports] == 0:5
        @test [p.symbol for p in ports] == ["gain", "bypass", "in_0", "in_1", "out_0", "out_1"]
        @test [p.audio for p in ports] == [false, false, true, true, true, true]
        @test [p.input for p in ports] == [true, true, true, true, false, false]
        @test last(AP.lv2_ports(look_spec)).latency
        src = read(only(AP.emit_wrapper(LV2(), gain_spec, mktempdir()).sources), String)
        @test !occursin(r"@[A-Z_]+@", src)
        @test occursin("#include \"fx_gain.h\"", src)
        @test occursin("#define AP_URI", src)
        @test occursin("\"urn:audioplugins:$(gain_spec.id)\"", src)
        @test occursin("fx_gain_step(x, true, &s->pars, &s->mem[c])", src)
        @test occursin("case 1: s->pars.bypass = (v != 0.0); break;", src)
    end

    @testset "a port symbol that collides is refused, not silently renamed" begin
        clash = PluginSpec(;
            id = "org.example.clash", name = "Clash", base = "fx_gain",
            pars = "FxGainPars", source = gain_spec.step.source,
            header = gain_spec.step.header, inputs = gain_spec.inputs,
            params = [
                PluginParam(; id = 0, name = "In", field = "in_0", min = 0, max = 1, default = 0),
            ]
        )
        @test_throws ArgumentError AP.lv2_ports(clash)
        @test_throws ArgumentError export_plugin(clash, joinpath(dir, "clash.lv2"); format = LV2())
    end

    @testset "lilv finds every bundle exported here, by URI" begin
        plugs = lv2_scan(LV2_PATH)
        uris = [p.uri for p in plugs]
        for spec in (gain_spec, eq_spec, look_spec, decim_spec)
            @test AP.lv2_plugin_uri(spec) in uris
            @test plugs[findfirst(==(AP.lv2_plugin_uri(spec)), uris)].name == spec.name
        end
    end

    @testset "the ports come back out of lilv exactly as declared" begin
        lv2_open!(
            LV2_PATH; uri = AP.lv2_plugin_uri(gain_spec), sample_rate = 48000,
            block_size = 64, channels = 2
        )
        @test lv2_plugin_uri() == AP.lv2_plugin_uri(gain_spec)
        @test lv2_plugin_name() == "Fixture Gain"
        @test AP.lv2_n_audio_in() == 2 && AP.lv2_n_audio_out() == 2
        ps = lv2_params()
        # The parameter id is the LV2 port index -- its position in the
        # descriptor -- and not the descriptor's own id, which is 0 and 7 here.
        @test [p.id for p in gain_spec.params] == [0, 7]
        @test [p.id for p in ps] == [0.0, 1.0]
        @test [p.symbol for p in ps] == [p.field for p in gain_spec.params]
        @test [p.name for p in ps] == [p.name for p in gain_spec.params]
        @test [(p.min, p.max, p.default) for p in ps] ==
            [(p.min, p.max, p.default) for p in gain_spec.params]
        @test lv2_param_value(0) == 1.0            # connected at its declared default
        @test isnan(lv2_param_value(2))            # an audio port is not a parameter
        @test lv2_latency() == 0.0
    end

    @testset "gain at 0.5: y == x * 0.5, sample-exactly, on both channels" begin
        x = signal(64)
        t = lv2_fill!(x)
        o = AP.lv2_process(t, 0, 0.5, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o; channel = 0) == x .* 0.5
        @test lv2_out(o; channel = 1) == x .* 0.5
        @test process(x, (0, 2.0)) == x .* 2
    end

    @testset "parameters are clamped, and a stepped bool rounds" begin
        x = signal(64)
        @test process(x, (0, 0.5), (1, 1.0)) == x            # bypassed
        @test lv2_param_value(1) == 1.0
        @test process(x, (0, 0.5), (1, 0.4)) == x .* 0.5     # 0.4 rounds to 0
        @test process(x, (0, 9.0), (1, 0.0)) == x .* 4       # clamped to the declared max
    end

    @testset "peaking EQ matches the RBJ recursion at the rate it was opened at" begin
        # LV2 control ports are floats, so the reference gets the parameter
        # values the plugin can actually have been given.
        f0, q, gdb = as_float(1000.0), as_float(0.7071067811865476), as_float(6.0)
        params = ((0, f0), (1, q), (2, gdb))
        eq_tol = 1.0e-6
        x = signal(256)
        open_eq(block) = lv2_open!(
            LV2_PATH; uri = AP.lv2_plugin_uri(eq_spec), sample_rate = 48000,
            block_size = block, channels = 2
        )
        open_eq(256)
        ps = lv2_params()
        @test [p.symbol for p in ps] == ["f0", "q", "gain_db"]
        @test [p.default for p in ps] == [as_float(p.default) for p in eq_spec.params]
        whole = process(x, params...)
        @test maximum(abs.(whole .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
        @test maximum(abs.(whole .- x)) > 1.0e-2          # the filter is doing something

        @testset "2 x 128 frames == 1 x 256 continuous, bitwise" begin
            open_eq(128)
            split = vcat(process(x[1:128], params...), process(x[129:256], params...))
            @test split == whole
            # Reopening is a fresh instance, so a run restarted at the boundary
            # must differ: that is what makes the equality above non-vacuous.
            open_eq(128)
            a = process(x[1:128], params...)
            open_eq(128)
            b = process(x[129:256], params...)
            @test vcat(a, b) != whole
        end

        @testset "the host's rate reaches the step function" begin
            outs = Vector{Float64}[]
            for fs in (44100, 48000, 96000)
                lv2_open!(
                    LV2_PATH; uri = AP.lv2_plugin_uri(eq_spec), sample_rate = fs,
                    block_size = 256, channels = 2
                )
                y = process(x, params...)
                @test maximum(abs.(y .- rbj_peaking(x, fs, f0, q, gdb))) < eq_tol
                push!(outs, y)
            end
            @test outs[1] != outs[2] && outs[2] != outs[3]
        end
    end

    @testset "a declared latency is surfaced on the designated port" begin
        lv2_open!(
            LV2_PATH; uri = AP.lv2_plugin_uri(look_spec), sample_rate = 48000,
            block_size = 64, channels = 2
        )
        @test look_spec.latency == 16
        # The designated port is written from activate(), so the host has the
        # latency before the first block, and it is still there after one.
        @test lv2_latency() == 16.0
        x = signal(64)
        y = process(x, (0, 1.0))
        @test lv2_latency() == 16.0
        @test all(iszero, y[1:16])
        @test y[17:64] == x[1:48]                  # the delay is real, not just declared
        @test length(lv2_params()) == 1            # the latency port is an output, not a parameter
        @test isnan(lv2_param_value(5))
    end

    @testset "a sub-clock output is held between ticks, across block boundaries" begin
        fresh(block) = lv2_open!(
            LV2_PATH; uri = AP.lv2_plugin_uri(decim_spec), sample_rate = 48000,
            block_size = block, channels = 2
        )
        x = signal(96)
        fresh(96)
        @test process(x, (0, 0.5), (1, 3.0)) == decimate_hold(x, 3, 0.5)
        fresh(96)
        @test !any(isnan, process(x, (0, 1.0), (1, 8.0)))   # NaN never leaks through the hold
        fresh(32)
        split = vcat((process(x[(32b + 1):(32b + 32)], (0, 1.0), (1, 5.0)) for b in 0:2)...)
        @test split == decimate_hold(x, 5, 1.0)
        # 32 is not a multiple of 5, so a plugin that restarted its phase at
        # each block would tick at the block boundary and differ.
        restarted = Float64[]
        for b in 0:2
            fresh(32)
            append!(restarted, process(x[(32b + 1):(32b + 32)], (0, 1.0), (1, 5.0)))
        end
        @test restarted != split
    end

    @testset "a mono descriptor, built and hosted as one channel" begin
        mono = PluginSpec(;
            id = "org.sciml.audioplugins.fixture.lv2mono", name = "Mono Gain",
            base = "fx_gain", pars = "FxGainPars", channels = 1,
            inputs = [("u", :audio), ("clock1", :clock)],
            source = gain_spec.step.source, header = gain_spec.step.header,
            params = [
                PluginParam(; id = 3, name = "Gain", field = "gain", min = 0, max = 2, default = 0.25),
            ]
        )
        out = export_plugin(mono, joinpath(dir, "mono.lv2"); format = LV2())
        @test [p.symbol for p in AP.lv2_ports(mono)] == ["gain", "in_0", "out_0"]
        lv2_open!(
            lv2_default_path(dirname(out)); uri = AP.lv2_plugin_uri(mono),
            sample_rate = 48000, block_size = 32, channels = 1
        )
        @test lv2_plugin_name() == "Mono Gain"
        @test only(lv2_params()).id == 0.0
        x = signal(32)
        @test process(x) == x .* 0.25               # the default applies untouched
        @test process(x, (0, 2.0)) == x .* 2
    end

    lv2_close!()

    @testset "LV2 authoring builds a C step only, and says so" begin
        jl_spec = spec_of("jl_gain.toml")
        @test jl_spec.step isa JuliaStep
        out = joinpath(dir, "jl_gain.lv2")
        err = try
            export_plugin(jl_spec, out; format = LV2())
            nothing
        catch e
            e
        end
        @test err isa ErrorException && occursin("CStep", err.msg)
        @test !ispath(out)
        @test_throws ErrorException AP.runtime_layout(LV2(), out)
    end
end
