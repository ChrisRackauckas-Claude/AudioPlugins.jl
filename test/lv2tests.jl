# The LV2 host, tested by hosting the LV2 bundle that ships with this package
# (test/plugins/ap_test_lv2.c + .ttl), discovered through lilv. The same
# arithmetic expectations as the CLAP suite, deliberately: the two hosts
# have the same shape, and this is what shows it.

using Test
using Libdl
using AudioPlugins
const AP = AudioPlugins

const LV2_DIR = lv2_test_bundle()               # directory containing ap_test.lv2
const LV2_PATH = lv2_default_path(LV2_DIR)       # + the spec bundles from lv2_jll
const LV2_MIDI_DIR = lv2_midi_test_bundle()      # directory containing ap_midi.lv2
const LV2_MIDI_PATH = lv2_default_path(LV2_MIDI_DIR)
const GAIN = "urn:audioplugins:test:gain"
const POLE = "urn:audioplugins:test:onepole"
const LOOK = "urn:audioplugins:test:lookahead"
const NG = "urn:audioplugins:test:notegain"

@testset "AudioPlugins / LV2" begin

    @testset "the host comes prebuilt, the test plugins do not" begin
        @test isfile(lv2_lib_path())
        @test !startswith(lv2_lib_path(), pkgdir(AudioPlugins))
        h = Libdl.dlopen(lv2_lib_path())
        @test Libdl.dlsym(h, :lv2_process) != C_NULL
        Libdl.dlclose(h)
        @test isfile(lv2_src_path())
        @test isdir(joinpath(LV2_DIR, "ap_test.lv2"))
        @test isfile(joinpath(LV2_DIR, "ap_test.lv2", "manifest.ttl"))
        @test !startswith(LV2_DIR, pkgdir(AudioPlugins))
        # The search path ends with lv2_jll's specification bundles.
        @test isfile(joinpath(last(split(LV2_PATH, AP.LV2_PATH_SEP)), "core.lv2", "manifest.ttl"))
    end

    @testset "discovery through lilv" begin
        plugs = lv2_scan(LV2_PATH)
        uris = [p.uri for p in plugs]
        @test length(plugs) == 3
        @test GAIN in uris && POLE in uris && LOOK in uris
        @test plugs[findfirst(==(GAIN), uris)].name == "AudioPlugins Test Gain"
        # A directory with no bundles is an error, not an empty answer.
        @test_throws ErrorException lv2_scan(mktempdir())
    end

    @testset "failure paths are loud" begin
        @test_throws ErrorException lv2_open!(LV2_PATH; uri = "urn:no:such", block_size = 64)
        @test occursin("urn:no:such", lv2_last_error())
        @test_throws ErrorException lv2_open!(LV2_PATH; uri = GAIN, block_size = 99999)
        @test_throws ErrorException lv2_open!(LV2_PATH; uri = GAIN, block_size = 64, channels = 7)
        @test_throws ErrorException lv2_open!(LV2_PATH; uri = GAIN, sample_rate = -1, block_size = 64)
        @test !lv2_is_open()
    end

    @testset "fewer audio outputs than channels repeats the last one" begin
        # Two host channels into a plugin with one audio output opens; the
        # single output is repeated on both channels (centre-panned).
        lv2_open!(LV2_PATH; uri = GAIN, block_size = 64, channels = 2)
        @test lv2_is_open() && lv2_channels() == 2
        o = AP.lv2_process(lv2_fill!(ones(64)), 0, 2.0, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o; channel = 0) ≈ fill(2.0, 64)
        @test lv2_out(o; channel = 1) ≈ fill(2.0, 64)
        lv2_close!()
    end

    @testset "open reports the configuration actually in force" begin
        lv2_open!(LV2_PATH; uri = GAIN, sample_rate = 48000, block_size = 64)
        @test lv2_is_open()
        @test lv2_block_size() == 64
        @test lv2_sample_rate() == 48000
        @test lv2_channels() == 1
        @test lv2_plugin_name() == "AudioPlugins Test Gain"
        @test lv2_plugin_uri() == GAIN
        @test AP.lv2_n_audio_in() == 1 && AP.lv2_n_audio_out() == 1
    end

    @testset "parameter discovery from the manifest" begin
        ps = lv2_params()
        @test length(ps) == 1
        @test ps[1].id == 0.0                    # port index
        @test ps[1].name == "Gain" && ps[1].symbol == "gain"
        @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
        @test lv2_param_value(0) == 1.0          # connected at its default
        @test isnan(lv2_param_value(1))          # an audio port is not a parameter
        @test isnan(lv2_param_value(77))
    end

    @testset "gain arithmetic: out == in * g, sample-exactly" begin
        lv2_reset_counters!()
        x = [sin(2pi * 5 * i / 64) * 0.5 for i in 0:63]
        for g in (0.5, 1.0, 2.0, 0.0)
            tok = lv2_fill!(x)
            out = AP.lv2_process(tok, 0, g, -1, 0, -1, 0, -1, 0)
            @test !isnan(out)
            y = lv2_out(out)
            @test length(y) == 64
            @test all(abs.(Float32.(x .* g) .- Float32.(y)) .< 1.0e-7)
        end
        @test lv2_n_process() == 4
    end

    @testset "a stale token is refused, not answered" begin
        t1 = lv2_fill!(ones(64))
        o1 = AP.lv2_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0)
        t2 = lv2_fill!(zeros(64))
        o2 = AP.lv2_process(t2, 0, 1.0, -1, 0, -1, 0, -1, 0)
        @test isnan(AP.lv2_out_rms(o1))
        @test !isnan(AP.lv2_out_rms(o2))
        @test isempty(lv2_out(o1))
        @test isnan(AP.lv2_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0))
        @test AP.lv2_out_valid(o1) == 0.0
        @test AP.lv2_out_valid(o2) == 1.0
    end

    @testset "parameter changes land on the intended block" begin
        t = lv2_fill!(fill(1.0, 64))
        o = AP.lv2_process(t, 0, 2.0, -1, 0, -1, 0, -1, 0)
        @test AP.lv2_out_peak(o) ≈ 2.0
        @test AP.lv2_out_rms(o) ≈ 2.0
        t = lv2_fill!(fill(1.0, 64))
        o = AP.lv2_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
        @test AP.lv2_out_peak(o) ≈ 0.25
        @test lv2_param_value(0) ≈ 0.25          # the port holds what was sent
        # NaN leaves a port alone; a negative id is an unused slot.
        t = lv2_fill!(fill(1.0, 64))
        o = AP.lv2_process(t, 0, NaN, -1, 0, -1, 0, -1, 0)
        @test AP.lv2_out_peak(o) ≈ 0.25
    end

    @testset "latency is reported (and documented as uncompensated)" begin
        @test lv2_latency() == 0.0
        lv2_open!(LV2_PATH; uri = LOOK, block_size = 64)
        @test lv2_latency() == 16.0
        @test lv2_param_count() == 0
        imp = zeros(64); imp[1] = 1.0
        t = lv2_fill!(imp)
        o = AP.lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        y = lv2_out(o)
        @test y[17] ≈ 1.0
        @test all(abs.(y[[1:16; 18:64]]) .< 1.0e-9)
        @test lv2_latency() == 16.0              # still, after a block
    end

    @testset "state persists across blocks (the invariant that matters)" begin
        a = 0.25
        lv2_open!(LV2_PATH; uri = POLE, block_size = 32)
        split = Float64[]
        for _ in 1:2
            t = lv2_fill!(ones(32))
            o = AP.lv2_process(t, 0, a, -1, 0, -1, 0, -1, 0)
            append!(split, lv2_out(o))
        end
        lv2_open!(LV2_PATH; uri = POLE, block_size = 64)
        t = lv2_fill!(ones(64))
        o = AP.lv2_process(t, 0, a, -1, 0, -1, 0, -1, 0)
        whole = lv2_out(o)
        @test length(split) == 64 && length(whole) == 64
        @test maximum(abs.(split .- whole)) < 1.0e-6
        @test split[32] ≈ 1 - (1 - a)^32 atol = 1.0e-5
        @test split[64] ≈ 1 - (1 - a)^64 atol = 1.0e-5
        @test split[33] > split[1]
    end

    @testset "reopening is a fresh instance" begin
        lv2_open!(LV2_PATH; uri = POLE, block_size = 32)
        t = lv2_fill!(ones(32))
        o = AP.lv2_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o)[1] ≈ 0.25 atol = 1.0e-6
    end

    @testset "closing is clean and idempotent" begin
        lv2_close!()
        @test !lv2_is_open()
        lv2_close!()
        @test !lv2_is_open()
        @test isnan(AP.lv2_process(1.0, 0, 1.0, -1, 0, -1, 0, -1, 0))
    end

    @testset "atom ports are discovered, sized and typed" begin
        lv2_open!(LV2_MIDI_PATH; uri = NG, block_size = 64)
        @test lv2_is_open()
        ports = lv2_atom_ports()
        @test length(ports) == 3
        @test (ports[1].index, ports[1].input, ports[1].midi) == (0, true, true)
        @test (ports[2].index, ports[2].input, ports[2].midi) == (1, false, true)
        @test (ports[3].index, ports[3].input, ports[3].midi) == (5, true, false)
        @test ports[1].size == 16384     # rsz:minimumSize honoured
        @test ports[2].size == 8192      # undeclared gets the host floor
        @test lv2_param_count() == 0     # a control output is not a parameter
        # No input block armed yet: queueing an event is an error.
        @test_throws ErrorException lv2_midi!(0, 0, 0x90, 60, 100)
    end

    @testset "MIDI events gate the output sample-exactly" begin
        # Note-on at frame 16 opens the gate at velocity 127/127 == 1.0;
        # a control change at 32 rides along and is echoed but changes
        # nothing.
        t = lv2_fill!(ones(64))
        lv2_midi!(0, 16, 0x90, 60, 127)
        lv2_midi!(0, 32, 0xB0, 1, 2)
        @test_throws ErrorException lv2_midi!(0, 8, 0x90, 60, 100)   # out of order
        @test_throws ErrorException lv2_midi!(0, 64, 0x90, 60, 100)  # frame >= block
        @test_throws ErrorException lv2_midi!(2, 0, 0x90, 60, 100)   # an audio port
        @test_throws ErrorException lv2_midi!(1, 0, 0x90, 60, 100)   # an atom output
        @test_throws ErrorException lv2_midi!(5, 0, 0x90, 60, 100)   # not MidiEvent
        o = AP.lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o) ≈ [zeros(16); ones(48)]
        @test lv2_port_value(4) == 2.0     # both events echoed to midi_out
        @test isnan(lv2_param_value(4))    # a control output is not a parameter
        @test isnan(lv2_port_value(2))     # an audio port has no port value

        # A block with no events: the gate stays open.
        o = AP.lv2_process(lv2_fill!(ones(64)), -1, 0, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o) ≈ ones(64)
        @test lv2_port_value(4) == 0.0

        # Note-off at frame 0 closes it; a velocity-64 note-on reopens at
        # gain 64/127.
        t = lv2_fill!(ones(64)); lv2_midi!(0, 0, 0x80, 60, 0)
        o = AP.lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        @test AP.lv2_out_peak(o) == 0.0
        t = lv2_fill!(ones(64)); lv2_midi!(0, 0, 0x90, 60, 64)
        o = AP.lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        @test lv2_out(o)[1] ≈ 64 / 127 atol = 1.0e-6
        lv2_close!()
    end

    # Real third-party bundles, when the machine has some: discovery must
    # enumerate them. This is the check the request asked for; it is
    # skipped rather than failed on a machine with no plugins installed.
    @testset "installed plugins enumerate" begin
        sysdirs = filter(
            isdir, Sys.iswindows() ? String[] :
                [
                    "/usr/lib/lv2", "/usr/local/lib/lv2", "/usr/lib/$(Sys.MACHINE)/lv2",
                    joinpath(homedir(), ".lv2"),
                ]
        )
        if isempty(sysdirs)
            @info "no system LV2 directories found; skipping the installed-plugin check"
        else
            plugs = lv2_scan(lv2_default_path(sysdirs...))
            @info "lilv found $(length(plugs)) installed LV2 plugin(s)" first(plugs, 5)
            @test length(plugs) > 0
            @test all(p -> !isempty(p.uri), plugs)
        end
        lv2_close!()
    end

end
