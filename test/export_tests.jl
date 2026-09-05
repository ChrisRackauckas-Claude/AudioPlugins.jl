# Authoring, proved with no code generator anywhere: the plugins built here
# come from the hand-written step functions under test/export/, and are then
# hosted by the same CLAP host the rest of the suite uses. If the seam is
# real, nothing on this path needs to know what generated the C.

using Test
using AudioPlugins
const AP = AudioPlugins

const FIX = joinpath(@__DIR__, "export")

# The reference recursion, written the way fx_eq.c writes it so the only
# difference is the host's float32 sample storage.
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

# Inputs that are exactly representable as float32, so that an exact
# comparison against the host's float32 buffers means what it says.
f32(x) = Float64.(Float32.(x))
signal(n) = f32([0.4sin(2pi * 0.013i) + 0.3sin(2pi * 0.171i) for i in 0:(n - 1)])

function process(x, params...)
    t = clap_fill!(x)
    slots = fill(-1.0, 8)
    for (k, (id, v)) in enumerate(params)
        slots[2k - 1], slots[2k] = id, v
    end
    o = AP.clp_process(t, slots...)
    isnan(o) && error("process failed: $(clap_last_error())")
    return clap_out(o)
end

@testset "AudioPlugins / export" begin

    gain_spec = read_plugin_spec(joinpath(FIX, "fx_gain.toml"))
    eq_spec = read_plugin_spec(joinpath(FIX, "fx_eq.toml"))

    @testset "the descriptor reads back" begin
        @test gain_spec.id == "org.sciml.audioplugins.fixture.gain"
        @test gain_spec.base == "fx_gain" && gain_spec.pars == "FxGainPars"
        @test [i.role for i in gain_spec.inputs] == [:audio, :clock]
        @test gain_spec.output == "y" && gain_spec.channels == 2
        @test gain_spec.features == ["audio-effect", "utility"]
        @test [p.id for p in gain_spec.params] == [0, 7]
        @test gain_spec.params[2].ctype == "bool" && gain_spec.params[2].stepped
        @test gain_spec.pkgconfig === nothing
        @test isabspath(gain_spec.source) && isfile(gain_spec.source)
        @test eq_spec.sample_rate_field == "fs"
        @test eq_spec.constants == ["enabled" => true]
        @test endswith(eq_spec.pkgconfig, "fx_eq.pc")
        @test plugin_format("clap") === CLAP()
    end

    @testset "the descriptor is validated" begin
        p(; kw...) = PluginParam(; id = 0, name = "P", field = "p", min = 0, max = 1, default = 0, kw...)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "1p", min = 0, max = 1, default = 0)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 0, max = 1, default = 2)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 1, max = 0, default = 0)
        @test_throws ArgumentError PluginParam(; id = 0, name = "P", field = "p", min = 0, max = Inf, default = 0)
        @test_throws ArgumentError p(ctype = "float")
        @test_throws ArgumentError StepInput("u", :midi)
        spec(; kw...) = PluginSpec(; id = "a", name = "b", base = "c", pars = "P",
                                   source = gain_spec.source, header = gain_spec.header, kw...)
        @test spec() isa PluginSpec
        @test_throws ArgumentError spec(inputs = [StepInput("c", :clock)])
        @test_throws ArgumentError spec(inputs = [StepInput("u", :audio), StepInput("v", :audio)])
        @test_throws ArgumentError spec(params = [p(), p()])
        @test_throws ArgumentError spec(source = "/nonexistent.c")
        @test_throws ArgumentError spec(base = "not an ident")
        @test_throws ArgumentError spec(constants = ["k" => "string"])
        @test_throws ArgumentError spec(channels = 0)
        @test_throws ArgumentError plugin_format("aax")
        @test_throws ArgumentError export_plugin(gain_spec, "out.vst3")
    end

    @testset "pkg-config files are read without pkg-config" begin
        mktempdir() do d
            pc = joinpath(d, "x.pc")
            write(pc, """
                prefix=/opt/x
                libdir=\${prefix}/lib   # a comment
                Name: x
                Cflags: -I\${prefix}/include -DX=1
                Libs: -L\${libdir} -lx
                Libs.private: -lm
                """)
            f = AP.pkgconfig_flags(pc)
            @test f.cflags == ["-I/opt/x/include", "-DX=1"]
            @test f.libs == ["-L/opt/x/lib", "-lx", "-lm"]
        end
    end

    @testset "the rendered wrapper" begin
        mktempdir() do d
            w = AP.emit_wrapper(CLAP(), gain_spec, d)
            src = read(only(w.sources), String)
            @test !occursin(r"@[A-Z_]+@", src)
            @test occursin("#include \"fx_gain.h\"", src)
            @test occursin("fx_gain_step(x, true, &s->pars, &s->mem[c])", src)
            @test occursin("case 0: s->pars.gain = v; break;", src)
            @test occursin("case 1: s->pars.bypass = (v != 0.0); break;", src)
            @test occursin("CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED", src)
            @test occursin("\"audio-effect\", \"utility\",", src)
            e = read(only(AP.emit_wrapper(CLAP(), eq_spec, d).sources), String)
            @test occursin("fx_eq_step(x, &s->pars, &s->mem[c])", e)
            @test occursin("s->pars.fs = sr;", e)
            @test occursin("s->pars.enabled = true;", e)
        end
    end

    dir = mktempdir()
    gain = export_plugin(gain_spec, joinpath(dir, "fx_gain.clap"))
    eq = export_plugin(eq_spec, joinpath(dir, "fx_eq.clap"))

    @testset "the bundle enumerates as described" begin
        @test ispath(gain) && ispath(eq)
        @test !startswith(gain, pkgdir(AudioPlugins))
        plugs = clap_scan(gain)
        @test length(plugs) == 1
        @test plugs[1].id == gain_spec.id && plugs[1].name == "Fixture Gain"
        clap_open!(gain; plugin_id = gain_spec.id, sample_rate = 48000, block_size = 64,
                   channels = 2)
        @test clap_plugin_name() == "Fixture Gain"
        @test clap_latency() == 0.0
        ps = clap_params()
        @test [p.id for p in ps] == [0.0, 7.0]
        @test ps[1].name == "Gain" && (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
        @test ps[2].name == "Bypass" && (ps[2].min, ps[2].max, ps[2].default) == (0.0, 1.0, 0.0)
    end

    @testset "gain at 0.5: y == x * 0.5, sample-exactly, on both channels" begin
        x = signal(64)
        t = clap_fill!(x)
        o = AP.clp_process(t, 0, 0.5, -1, 0, -1, 0, -1, 0)
        @test clap_out(o; channel = 0) == x .* 0.5
        @test clap_out(o; channel = 1) == x .* 0.5
        @test maximum(abs.(clap_out(o) .- x .* 0.5)) == 0.0
        @test process(x, (0, 2.0)) == x .* 2
    end

    @testset "parameters are clamped, and a stepped bool rounds" begin
        x = signal(64)
        @test process(x, (0, 0.5), (7, 1.0)) == x            # bypassed
        @test AP.clap_param_value(7) == 1.0
        @test process(x, (0, 0.5), (7, 0.4)) == x .* 0.5     # 0.4 rounds to 0
        @test AP.clap_param_value(7) == 0.0
        @test process(x, (0, 9.0), (7, 0.0)) == x .* 4       # clamped to max
        @test AP.clap_param_value(0) == 4.0
    end

    f0, q, gdb = 1000.0, 0.7071067811865476, 6.0
    eq_params = ((0, f0), (1, q), (2, gdb))
    # The host stores samples as float32, so the reference can only be met to
    # one float32 ulp of the output (about 6e-8 relative); 1e-6 absolute is
    # well inside that for a signal of unit magnitude and would catch a wrong
    # coefficient or a lost state term.
    eq_tol = 1e-6

    @testset "peaking EQ matches the RBJ recursion" begin
        clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 256)
        ps = clap_params()
        @test [p.id for p in ps] == [0.0, 1.0, 2.0]
        @test [p.default for p in ps] == [f0, q, gdb]
        x = signal(256)
        y = process(x, eq_params...)
        ref = rbj_peaking(x, 48000, f0, q, gdb)
        @test maximum(abs.(y .- ref)) < eq_tol
        @test maximum(abs.(y .- x)) > 1e-2      # the filter is doing something
    end

    @testset "2 x 128 frames == 1 x 256 continuous, bitwise" begin
        x = signal(256)
        clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
        split = vcat(process(x[1:128], eq_params...), process(x[129:256], eq_params...))
        clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 256)
        whole = process(x, eq_params...)
        @test split == whole
        @test maximum(abs.(whole .- rbj_peaking(x, 48000, f0, q, gdb))) < eq_tol
        # Reopening is a fresh instance, so a run restarted at the boundary
        # must differ: that is what makes the equality above non-vacuous.
        clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
        a = process(x[1:128], eq_params...)
        clap_open!(eq; plugin_id = eq_spec.id, sample_rate = 48000, block_size = 128)
        b = process(x[129:256], eq_params...)
        @test vcat(a, b) != whole
    end

    @testset "activate(sr) at 44.1 / 48 / 96 kHz gives three correct filters" begin
        x = signal(256)
        outs = Vector{Float64}[]
        for fs in (44100, 48000, 96000)
            clap_open!(eq; plugin_id = eq_spec.id, sample_rate = fs, block_size = 256)
            y = process(x, eq_params...)
            @test maximum(abs.(y .- rbj_peaking(x, fs, f0, q, gdb))) < eq_tol
            push!(outs, y)
        end
        @test outs[1] != outs[2] && outs[2] != outs[3]
    end

    @testset "link flags come from the .pc file" begin
        # Without fx_eq.pc's `Libs: -lm` the bundle has undefined sin/cos/pow,
        # and the link refuses it rather than leaving it for the host's dlopen.
        # macOS links libm through libSystem regardless, so only ELF can check.
        if Sys.islinux()
            nopc = PluginSpec(; id = eq_spec.id, name = eq_spec.name, base = eq_spec.base,
                              pars = eq_spec.pars, source = eq_spec.source,
                              header = eq_spec.header, inputs = eq_spec.inputs,
                              sample_rate_field = "fs", params = eq_spec.params,
                              constants = eq_spec.constants)
            @test_throws ProcessFailedException redirect_stderr(devnull) do
                export_plugin(nopc, joinpath(dir, "fx_eq_nopc.clap"))
            end
        end
    end

    @testset "a mono descriptor and a Julia-side spec, no TOML" begin
        mono = PluginSpec(; id = "org.sciml.audioplugins.fixture.mono", name = "Mono Gain",
                          base = "fx_gain", pars = "FxGainPars", channels = 1,
                          inputs = [("u", :audio), ("clock1", :clock)],
                          source = gain_spec.source, header = gain_spec.header,
                          params = [PluginParam(; id = 3, name = "Gain", field = "gain",
                                                min = 0, max = 2, default = 0.25)])
        b = export_plugin(mono, joinpath(dir, "mono.clap"))
        clap_open!(b; block_size = 32)
        @test clap_plugin_name() == "Mono Gain"
        @test only(clap_params()).id == 3.0
        x = signal(32)
        @test process(x) == x .* 0.25               # the default applies untouched
        @test process(x, (3, 2.0)) == x .* 2
    end

    clap_close!()
end
