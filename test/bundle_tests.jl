# The bundle registry. Everything here runs against the package's own test
# bundle: the point being checked is the bookkeeping -- what is registered,
# what `plugins()` says about it, and that an id resolves to the right module
# -- not the arithmetic, which clap_host_tests.jl already pins down.

using Test
using AudioPlugins

# A second copy of the same bundle at a different path, for the case two
# collections declare the same plugin id. Copying is the honest way to make
# that collision: it is exactly what two JLLs built from one upstream would do.
function second_copy(dir)
    src = clap_test_bundle()
    dst = joinpath(dir, "ap_test_copy.clap")
    cp(src, dst; force = true)
    return dst
end

@testset "AudioPlugins / bundle registry" begin
    bundle = clap_test_bundle()
    # Registration is global state, so every testset below leaves it empty.
    empty_registry!() = foreach(b -> unregister_bundle!(b.path), bundles())

    @testset "nothing is registered until something registers it" begin
        empty_registry!()
        @test isempty(bundles())
        @test isempty(plugins())
        # An id no bundle declares resolves to nothing rather than guessing.
        @test AudioPlugins.find_plugin("ap.gain") === nothing
    end

    @testset "registering a bundle records every plugin in it" begin
        empty_registry!()
        @test register_bundle!(bundle) == 3

        bs = bundles()
        @test length(bs) == 1
        @test bs[1].path == abspath(bundle)
        @test bs[1].n == 3
        @test bs[1].source === nothing

        ps = plugins()
        @test [p.id for p in ps] == ["ap.gain", "ap.onepole", "ap.lookahead"]
        @test ps[1].name == "AudioPlugins Test Gain"
        # Factory order, zero-based, as the host reports it.
        @test [p.index for p in ps] == [0, 1, 2]
        @test all(p -> p.bundle == abspath(bundle), ps)

        @test plugins(bundle) == ps
        @test isempty(plugins(joinpath(dirname(bundle), "nothing_here.clap")))
    end

    @testset "a bundle can name the module it came from" begin
        empty_registry!()
        # Any module serves: the registry stores the label and never loads it.
        register_bundle!(bundle; source = AudioPlugins)
        @test bundles()[1].source === AudioPlugins
        @test length(plugins(AudioPlugins)) == 3
        @test isempty(plugins(Test))
    end

    @testset "re-registering replaces rather than duplicates" begin
        empty_registry!()
        register_bundle!(bundle)
        register_bundle!(bundle)
        @test length(bundles()) == 1
        @test length(plugins()) == 3
    end

    @testset "unregistering" begin
        empty_registry!()
        register_bundle!(bundle)
        @test unregister_bundle!(bundle) == true
        @test unregister_bundle!(bundle) == false   # already gone
        @test isempty(plugins())
    end

    @testset "a bundle that cannot be scanned is refused at registration" begin
        empty_registry!()
        @test_throws ErrorException register_bundle!(joinpath(@__DIR__, "no_such.clap"))
        @test isempty(bundles())
    end

    @testset "registering closes whatever was open" begin
        empty_registry!()
        clap_open!(bundle; plugin_id = "ap.gain", block_size = 64)
        @test clap_is_open()
        register_bundle!(bundle)
        # The host holds one module at a time, so a scan has to evict it. The
        # docstring warns about this; the test is what keeps the warning true.
        @test !clap_is_open()
        clap_close!()
    end

    @testset "opening by id alone" begin
        empty_registry!()
        register_bundle!(bundle)

        clap_open!("ap.gain"; block_size = 64, sample_rate = 48000)
        @test clap_is_open()
        @test clap_plugin_name() == "AudioPlugins Test Gain"
        @test clap_block_size() == 64

        # And it is really that plugin, not merely a plugin: gain 0.5 halves.
        # Compared in Float32 because the host's buffers are, as elsewhere.
        x = collect(range(-1.0, 1.0; length = 64))
        tok = AudioPlugins.clp_process(clap_fill!(x), 0, 0.5, -1, 0, -1, 0, -1, 0)
        @test Float32.(clap_out(tok)) == Float32.(0.5 .* x)

        # An id from further down the same bundle resolves too.
        clap_open!("ap.lookahead"; block_size = 64)
        @test clap_latency() == 16
        clap_close!()
    end

    @testset "a path still wins, and an unknown string still reaches the host" begin
        empty_registry!()
        register_bundle!(bundle)

        # Explicit path plus plugin_id: the pre-registry call, unchanged.
        clap_open!(bundle; plugin_id = "ap.onepole", block_size = 32)
        @test clap_plugin_name() == "AudioPlugins Test One Pole"
        clap_close!()

        # A string that is neither a path nor a registered id is passed to the
        # host as a path, so the error is the host's own, as it always was.
        err = try
            clap_open!("definitely.not.a.plugin"; block_size = 64)
            nothing
        catch e
            sprint(showerror, e)
        end
        @test err !== nothing
        @test occursin("definitely.not.a.plugin", err)
    end

    @testset "an id two bundles both declare is refused, not guessed" begin
        mktempdir() do dir
            empty_registry!()
            copy = second_copy(dir)
            register_bundle!(bundle)
            register_bundle!(copy)

            @test length(bundles()) == 2
            @test length(plugins()) == 6

            # Which of the two you got would otherwise depend on load order.
            @test_throws ErrorException AudioPlugins.find_plugin("ap.gain")
            @test_throws ErrorException clap_open!("ap.gain"; block_size = 64)

            # Saying which bundle you mean is always available, and works.
            clap_open!(copy; plugin_id = "ap.gain", block_size = 64)
            @test clap_is_open()
            clap_close!()
        end
    end

    empty_registry!()
end
