# What this sublibrary is for is exposing the LV2 bundle directory: that
# `using X42PluginsGPL3` alone is enough to find the thirteen plugins under
# `lv2_default_path(lv2_dir())`. The DSP itself is third-party, so what is
# checked is the scan, that every plugin this host can instantiate opens and
# turns a sine into finite, non-silent output, and that the zconvo plugins are
# refused for the host features they require.

using Test
using AudioPlugins
using X42PluginsGPL3
using X42PluginsGPL3_jll

const OPENABLE = [
    ("http://gareus.org/oss/lv2/darc#mono", 1),
    ("http://gareus.org/oss/lv2/darc#stereo", 2),
    ("http://gareus.org/oss/lv2/dpl#mono", 1),
    ("http://gareus.org/oss/lv2/dpl#stereo", 2),
    ("http://gareus.org/oss/lv2/fat1", 1),
    ("http://gareus.org/oss/lv2/fat1#microtonal", 1),
    ("http://gareus.org/oss/lv2/fat1#scales", 1),
]

const ZCONVO = [
    "http://gareus.org/oss/lv2/zeroconvolv#CfgMono",
    "http://gareus.org/oss/lv2/zeroconvolv#CfgMonoToStereo",
    "http://gareus.org/oss/lv2/zeroconvolv#CfgStereo",
    "http://gareus.org/oss/lv2/zeroconvolv#Mono",
    "http://gareus.org/oss/lv2/zeroconvolv#MonoToStereo",
    "http://gareus.org/oss/lv2/zeroconvolv#Stereo",
]

const EXPECTED_URIS = sort!([first.(OPENABLE); ZCONVO])

@testset "X42PluginsGPL3" begin
    path = lv2_default_path(lv2_dir())

    @testset "lv2_dir points at the JLL lv2 root" begin
        @test isdir(lv2_dir())
        @test basename(lv2_dir()) == "lv2"
        @test isfile(X42PluginsGPL3_jll.darc_lv2)
        @test dirname(dirname(X42PluginsGPL3_jll.darc_lv2)) == lv2_dir()
        @test sort(readdir(lv2_dir())) ==
            ["darc.lv2", "dpl.lv2", "fat1.lv2", "zeroconvo.lv2"]
    end

    @testset "scan finds exactly the thirteen shipped plugins" begin
        plugs = lv2_scan(path)
        x42 = sort([p.uri for p in plugs if occursin("gareus.org", p.uri)])
        @test length(x42) == 13
        @test x42 == EXPECTED_URIS
    end

    sr, n, nblocks = 48000, 256, 16
    @testset "$uri opens and processes audio" for (uri, ch) in OPENABLE
        lv2_open!(path; uri = uri, sample_rate = sr, block_size = n, channels = ch)
        @test lv2_is_open()
        @test lv2_plugin_uri() == uri
        peak = 0.0
        for b in 0:(nblocks - 1)
            x = [
                0.5 * sin(2pi * 440 * (b * n + i) / sr) for i in 0:(n - 1) for _ in 1:ch
            ]
            tok = AudioPlugins.lv2_process(
                lv2_fill!(x; channels = ch), -1, 0, -1, 0, -1, 0, -1, 0
            )
            for c in 0:(ch - 1)
                y = lv2_out(tok; channel = c)
                @test length(y) == n
                @test all(isfinite, y)
                peak = max(peak, maximum(abs, y))
            end
        end
        @test peak > 0.01
        lv2_close!()
    end

    @testset "$uri is refused for the host features it requires" for uri in ZCONVO
        @test_throws r"worker#schedule|options#options" lv2_open!(
            path; uri = uri, sample_rate = sr, block_size = n, channels = 1
        )
        @test !lv2_is_open()
    end
end
