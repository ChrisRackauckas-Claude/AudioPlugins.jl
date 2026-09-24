# What this sublibrary is for is exposing the LV2 bundle directory: that
# `using X42Plugins` alone is enough to find fifty-four plugins under
# `lv2_default_path(lv2_dir())`. The arithmetic of the plugins themselves is
# not ours to assert -- they are third-party DSP -- so what is checked here is
# the scan count, then that at least one audio plugin and one MIDI plugin open
# and process through this package's own host.

using Test
using AudioPlugins
using X42Plugins
using X42Plugins_jll

const EXPECTED_URIS = [
    "http://gareus.org/oss/lv2/balance",
    "http://gareus.org/oss/lv2/controlfilter#exp",
    "http://gareus.org/oss/lv2/controlfilter#invert",
    "http://gareus.org/oss/lv2/controlfilter#linearscale",
    "http://gareus.org/oss/lv2/controlfilter#lowpass",
    "http://gareus.org/oss/lv2/controlfilter#nlog",
    "http://gareus.org/oss/lv2/matrixmixer#i8o8",
    "http://gareus.org/oss/lv2/mididebug",
    "http://gareus.org/oss/lv2/midifilter#cctonote",
    "http://gareus.org/oss/lv2/midifilter#channelfilter",
    "http://gareus.org/oss/lv2/midifilter#channelmap",
    "http://gareus.org/oss/lv2/midifilter#chokefilter",
    "http://gareus.org/oss/lv2/midifilter#enforcescale",
    "http://gareus.org/oss/lv2/midifilter#eventblocker",
    "http://gareus.org/oss/lv2/midifilter#keyrange",
    "http://gareus.org/oss/lv2/midifilter#keysplit",
    "http://gareus.org/oss/lv2/midifilter#mapcc",
    "http://gareus.org/oss/lv2/midifilter#mapkeychannel",
    "http://gareus.org/oss/lv2/midifilter#mapkeyscale",
    "http://gareus.org/oss/lv2/midifilter#midichord",
    "http://gareus.org/oss/lv2/midifilter#mididelay",
    "http://gareus.org/oss/lv2/midifilter#mididup",
    "http://gareus.org/oss/lv2/midifilter#midistrum",
    "http://gareus.org/oss/lv2/midifilter#miditranspose",
    "http://gareus.org/oss/lv2/midifilter#monolegato",
    "http://gareus.org/oss/lv2/midifilter#noactivesensing",
    "http://gareus.org/oss/lv2/midifilter#nodup",
    "http://gareus.org/oss/lv2/midifilter#notetocc",
    "http://gareus.org/oss/lv2/midifilter#notetoggle",
    "http://gareus.org/oss/lv2/midifilter#notetopgm",
    "http://gareus.org/oss/lv2/midifilter#ntapdelay",
    "http://gareus.org/oss/lv2/midifilter#onechannelfilter",
    "http://gareus.org/oss/lv2/midifilter#passthru",
    "http://gareus.org/oss/lv2/midifilter#quantize",
    "http://gareus.org/oss/lv2/midifilter#randvelocity",
    "http://gareus.org/oss/lv2/midifilter#scalecc",
    "http://gareus.org/oss/lv2/midifilter#sostenuto",
    "http://gareus.org/oss/lv2/midifilter#tonalpedal",
    "http://gareus.org/oss/lv2/midifilter#velocitygamma",
    "http://gareus.org/oss/lv2/midifilter#velocityrange",
    "http://gareus.org/oss/lv2/midifilter#velocityscale",
    "http://gareus.org/oss/lv2/midigen",
    "http://gareus.org/oss/lv2/midimap",
    "http://gareus.org/oss/lv2/nodelay",
    "http://gareus.org/oss/lv2/nodelay#mega",
    "http://gareus.org/oss/lv2/nodelay#micro",
    "http://gareus.org/oss/lv2/onsettrigger#bassdrum_mono",
    "http://gareus.org/oss/lv2/onsettrigger#bassdrum_stereo",
    "http://gareus.org/oss/lv2/phaserotate",
    "http://gareus.org/oss/lv2/phaserotate#stereo",
    "http://gareus.org/oss/lv2/stepseq#s8n8",
    "http://gareus.org/oss/lv2/stereoroute",
    "http://gareus.org/oss/lv2/testsignal",
    "http://gareus.org/oss/lv2/xfade",
]

@testset "X42Plugins" begin
    path = lv2_default_path(lv2_dir())

    @testset "lv2_dir points at the JLL lib/lv2" begin
        @test isdir(lv2_dir())
        @test endswith(lv2_dir(), joinpath("lib", "lv2")) || endswith(replace(lv2_dir(), "\\" => "/"), "lib/lv2")
        @test isfile(X42Plugins_jll.balance_lv2)
        @test dirname(dirname(X42Plugins_jll.balance_lv2)) == lv2_dir()
    end

    @testset "scan finds exactly the fifty-four shipped plugins" begin
        plugs = lv2_scan(path)
        x42 = sort([p.uri for p in plugs if occursin("gareus.org", p.uri)])
        @test length(x42) == 54
        @test x42 == EXPECTED_URIS
        # midimap is present for hosts that offer worker:schedule; this host does not.
        @test "http://gareus.org/oss/lv2/midimap" in x42
    end

    @testset "nodelay opens and processes audio" begin
        uri = "http://gareus.org/oss/lv2/nodelay"
        lv2_open!(path; uri = uri, sample_rate = 48000, block_size = 64, channels = 1)
        @test lv2_is_open()
        x = Float64[0.9 * sin(2pi * 440 * i / 48000) for i in 0:63]
        tok = AudioPlugins.lv2_process(lv2_fill!(x; channels = 1), -1, 0, -1, 0, -1, 0, -1, 0)
        y = lv2_out(tok)
        @test length(y) == 64
        @test all(isfinite, y)
        lv2_close!()
    end

    @testset "a MIDI filter opens and accepts a note-on" begin
        uri = "http://gareus.org/oss/lv2/midifilter#passthru"
        lv2_open!(path; uri = uri, sample_rate = 48000, block_size = 64, channels = 1)
        @test lv2_is_open()
        ports = AudioPlugins.lv2_atom_ports()
        midi_in = only(p for p in ports if p.input && p.midi)
        t = lv2_fill!(zeros(64); channels = 1)
        AudioPlugins.lv2_midi!(t, midi_in.index, 0, 0x90, 60, 100)
        tok = AudioPlugins.lv2_process(t, -1, 0, -1, 0, -1, 0, -1, 0)
        @test length(lv2_out(tok)) == 64
        lv2_close!()
    end

    @testset "midimap is refused for lack of worker:schedule" begin
        uri = "http://gareus.org/oss/lv2/midimap"
        @test_throws ErrorException lv2_open!(path; uri = uri, sample_rate = 48000, block_size = 64, channels = 1)
    end
end
