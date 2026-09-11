# The LV2 host, tested by hosting plugins that ship with this package, exactly
# as clap_host_tests.jl does and for the same reason: hosting is only proved by
# hosting something, and `test/plugins/ap_test_lv2.c` is ours, so every
# expectation below is arithmetic rather than a recording.
#
# The same three plugins as the CLAP bundle -- a gain, a one-pole and a
# 16-sample lookahead -- so the two suites check the same invariants against
# two plugin ABIs. test/probe_lv2.c is the C-level twin of this file.

using Test
using Libdl
using AudioPlugins
const AP = AudioPlugins

const GAIN = "urn:audioplugins:test:gain"
const POLE = "urn:audioplugins:test:onepole"
const LOOK = "urn:audioplugins:test:lookahead"

if !lv2_host_available()
    @testset "AudioPlugins / LV2: no prebuilt host on this platform" begin
        # A platform LV2Host_jll has no build for. Hosting is covered by
        # test/probe_lv2.c, which compiles csrc/lv2_host.c against lilv.
        @test isfile(lv2_src_path())
        @test_throws ErrorException lv2_lib_path()
        @info "LV2Host_jll has no build for $(Base.BinaryPlatforms.host_triplet()): " *
            "in-process hosting tests do not run here"
    end
else
    BUNDLE_DIR = lv2_test_bundle()

    @testset "AudioPlugins / LV2" begin

        @testset "the host comes prebuilt, the test plugins do not" begin
            @test isfile(lv2_lib_path())
            @test !startswith(lv2_lib_path(), pkgdir(AudioPlugins))
            h = Libdl.dlopen(lv2_lib_path())
            @test Libdl.dlsym(h, :lv2_process) != C_NULL
            Libdl.dlclose(h)
            @test isfile(lv2_src_path())
            @test endswith(lv2_src_path(), joinpath("csrc", "lv2_host.c"))
            @test !startswith(BUNDLE_DIR, pkgdir(AudioPlugins))
            @test isfile(joinpath(BUNDLE_DIR, "ap_test.lv2", "manifest.ttl"))
        end

        @testset "the bundle builds and enumerates" begin
            plugs = lv2_scan(BUNDLE_DIR)
            @test length(plugs) == 3
            @test sort([p.uri for p in plugs]) == sort([GAIN, POLE, LOOK])
            @test first(p.name for p in plugs if p.uri == GAIN) ==
                "AudioPlugins Test Gain"
            # A vector of directories is joined with the platform's separator.
            @test lv2_scan([BUNDLE_DIR]) == plugs
        end

        @testset "failure paths are loud" begin
            @test_throws ErrorException lv2_scan(joinpath(BUNDLE_DIR, "nonexistent"))
            @test !isempty(lv2_last_error())
            @test_throws ErrorException lv2_open!(
                BUNDLE_DIR; uri = "urn:no:such",
                block_size = 64
            )
            @test occursin("urn:no:such", lv2_last_error())
            @test_throws ErrorException lv2_open!(BUNDLE_DIR; uri = GAIN, block_size = 99999)
            @test_throws ErrorException lv2_open!(
                BUNDLE_DIR; uri = GAIN, block_size = 64,
                channels = 7
            )
            # A mono plugin cannot serve two host channels: it has one audio output.
            @test_throws ErrorException lv2_open!(
                BUNDLE_DIR; uri = GAIN, block_size = 64,
                channels = 2
            )
            @test occursin("audio output", lv2_last_error())
            @test_throws ErrorException lv2_open!(
                BUNDLE_DIR; uri = GAIN, sample_rate = -1,
                block_size = 64
            )
            # State after a failed open is closed, not half-open.
            @test !lv2_is_open()
        end

        @testset "open reports the configuration actually in force" begin
            lv2_open!(BUNDLE_DIR; uri = GAIN, sample_rate = 48000, block_size = 64)
            @test lv2_is_open()
            @test lv2_block_size() == 64
            @test lv2_sample_rate() == 48000
            @test lv2_channels() == 1
            @test lv2_n_audio_in() == 1 && lv2_n_audio_out() == 1
            @test lv2_plugin_name() == "AudioPlugins Test Gain"
            @test lv2_plugin_uri() == GAIN
        end

        @testset "parameter discovery: a parameter is a control input port" begin
            ps = lv2_params()
            @test length(ps) == 1
            @test lv2_param_count() == 1
            @test ps[1].id == 0.0            # the port index, not an opaque id
            @test ps[1].name == "Gain"
            @test ps[1].symbol == "gain"
            @test (ps[1].min, ps[1].max, ps[1].default) == (0.0, 4.0, 1.0)
            @test AP.lv2_param_value(0) == 1.0    # connected at its default
            @test isnan(AP.lv2_param_value(1))    # an audio port is not a parameter
        end

        @testset "gain arithmetic: out == in * g, sample-exactly" begin
            lv2_reset_counters!()
            x = [sin(2pi * 5 * i / 64) * 0.5 for i in 0:63]
            for g in (0.5, 1.0, 2.0, 0.0)
                tok = lv2_fill!(x)
                out = AP.lv2p_process(tok, 0, g, -1, 0, -1, 0, -1, 0)
                @test !isnan(out)
                y = lv2_out(out)
                @test length(y) == 64
                # float32 storage inside the plugin, so compare at float precision
                @test all(abs.(Float32.(x .* g) .- Float32.(y)) .< 1.0e-7)
            end
            @test lv2_n_process() == 4     # exactly one run() per call
        end

        @testset "the node-side tone source agrees with the readers" begin
            tok = AP.lv2p_in_tone(1.0, LV2_WAVE_SINE, 1000.0, 0.5)
            @test !isnan(tok)
            out = AP.lv2p_process(tok, 0, 1.0, -1, 0, -1, 0, -1, 0)
            @test AP.lv2p_out_count(out) == 64
            @test AP.lv2p_out_peak(out) ≈ 0.5 atol = 1.0e-3
            for i in (0, 1, 63)
                @test AP.lv2p_out_sample(out, i, 0) ≈ AP.lv2p_in_sample(tok, i, 0) atol = 1.0e-7
            end
            @test !isnan(AP.lv2p_in_tone(1.0, LV2_WAVE_SILENCE, 1000.0, 0.5))
        end

        @testset "a stale token is refused, not answered" begin
            t1 = lv2_fill!(ones(64))
            o1 = AP.lv2p_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0)
            t2 = lv2_fill!(zeros(64))
            o2 = AP.lv2p_process(t2, 0, 1.0, -1, 0, -1, 0, -1, 0)
            @test isnan(AP.lv2p_out_rms(o1))            # superseded output token
            @test !isnan(AP.lv2p_out_rms(o2))           # the current one still reads
            @test isempty(lv2_out(o1))                  # and the Julia reader agrees
            @test isnan(AP.lv2p_process(t1, 0, 1.0, -1, 0, -1, 0, -1, 0))
            @test AP.lv2p_out_valid(o1) == 0.0
            @test AP.lv2p_out_valid(o2) == 1.0
        end

        @testset "parameter changes land on the intended block" begin
            t = lv2_fill!(fill(1.0, 64))
            o = AP.lv2p_process(t, 0, 2.0, -1, 0, -1, 0, -1, 0)
            @test AP.lv2p_out_peak(o) ≈ 2.0
            @test AP.lv2p_out_rms(o) ≈ 2.0
            t = lv2_fill!(fill(1.0, 64))
            o = AP.lv2p_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
            @test AP.lv2p_out_peak(o) ≈ 0.25
            # And the port really holds what was sent.
            @test AP.lv2_param_value(0) ≈ 0.25
            @test isnan(AP.lv2_param_value(77))
        end

        @testset "latency comes from the designated port (and is uncompensated)" begin
            lv2_open!(BUNDLE_DIR; uri = GAIN, block_size = 64)
            @test lv2_latency() == 0.0
            lv2_open!(BUNDLE_DIR; uri = LOOK, block_size = 64)
            @test lv2_latency() == 16.0
            @test lv2_param_count() == 0        # lookahead exposes no parameters
            # The delay is real: a unit impulse comes out 16 samples later.
            imp = zeros(64); imp[1] = 1.0
            t = lv2_fill!(imp)
            o = AP.lv2p_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
            y = lv2_out(o)
            @test y[17] ≈ 1.0                   # 1-based: sample 16 -> index 17
            @test all(abs.(y[[1:16; 18:64]]) .< 1.0e-9)
        end

        @testset "state persists across blocks (the invariant that matters)" begin
            # Two consecutive 32-frame blocks through the one-pole must equal one
            # 64-frame run over the concatenated input. If the plugin's state reset
            # per block, or the host re-activated between blocks, the second half
            # would restart from zero.
            a = 0.25
            lv2_open!(BUNDLE_DIR; uri = POLE, block_size = 32)
            split = Float64[]
            for _ in 1:2
                t = lv2_fill!(ones(32))
                o = AP.lv2p_process(t, 0, a, -1, 0, -1, 0, -1, 0)
                append!(split, lv2_out(o))
            end
            lv2_open!(BUNDLE_DIR; uri = POLE, block_size = 64)
            t = lv2_fill!(ones(64))
            o = AP.lv2p_process(t, 0, a, -1, 0, -1, 0, -1, 0)
            whole = lv2_out(o)

            @test length(split) == 64 && length(whole) == 64
            @test maximum(abs.(split .- whole)) < 1.0e-6
            # Closed form: a one-pole step response is 1 - (1-a)^n.
            @test split[32] ≈ 1 - (1 - a)^32 atol = 1.0e-5
            @test split[64] ≈ 1 - (1 - a)^64 atol = 1.0e-5
            # And the state really is carried: block 2 starts above where block 1 began.
            @test split[33] > split[1]
        end

        @testset "reopening is a fresh instance" begin
            # The counterpart of the test above, and what makes that one meaningful.
            lv2_open!(BUNDLE_DIR; uri = POLE, block_size = 32)
            t = lv2_fill!(ones(32))
            o = AP.lv2p_process(t, 0, 0.25, -1, 0, -1, 0, -1, 0)
            @test lv2_out(o)[1] ≈ 0.25 atol = 1.0e-6     # y = 0 + 0.25*(1-0)
        end

        @testset "closing is clean and idempotent" begin
            lv2_close!()
            @test !lv2_is_open()
            lv2_close!()
            @test !lv2_is_open()
            @test lv2_plugin_uri() == ""
            @test isnan(AP.lv2p_process(1.0, 0, 1.0, -1, 0, -1, 0, -1, 0))
        end

    end
end # lv2_host_available()
