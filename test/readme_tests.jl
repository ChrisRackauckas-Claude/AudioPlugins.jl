# The README's examples, run as written. A quoted call that no longer exists
# is invisible to every other test in this suite, so it is checked here
# instead: each ```julia block is executed in a module of its own, in a
# temporary directory, and anything it throws fails the suite.
#
# Blocks that are sketches rather than programs -- a path that does not
# exist, a fragment shown for its shape -- carry an `<!-- illustrative -->`
# marker line directly above the fence and are skipped. The marker is opt-out
# and visible in the source, so a new example is runnable by default.

using Test
using AudioPlugins

const README = normpath(joinpath(@__DIR__, "..", "README.md"))
const ILLUSTRATIVE = "<!-- illustrative -->"

function readme_examples(md::AbstractString)
    lines = split(md, r"\r?\n")
    blocks = NamedTuple{(:line, :code), Tuple{Int, String}}[]
    i = firstindex(lines)
    while i <= lastindex(lines)
        if strip(lines[i]) == "```julia"
            close = findnext(l -> strip(l) == "```", lines, i + 1)
            close === nothing && error("$README: unterminated ```julia fence at line $i")
            if !(i > 1 && strip(lines[i - 1]) == ILLUSTRATIVE)
                push!(blocks, (line = i, code = join(lines[(i + 1):(close - 1)], "\n")))
            end
            i = close + 1
        else
            i += 1
        end
    end
    return blocks
end

@testset "AudioPlugins / README examples" begin
    examples = readme_examples(read(README, String))
    @test !isempty(examples)

    if clap_host_available()
        for (n, ex) in enumerate(examples)
            mod = Module(Symbol("ReadmeExample", n))
            mktempdir() do dir
                cd(dir) do
                    @test (include_string(mod, ex.code, "$README:$(ex.line)"); true)
                end
            end
            clap_close!()
        end
    else
        @info "no prebuilt CLAP host for $(Base.BinaryPlatforms.host_triplet()): " *
              "README examples are extracted but not run here"
    end
end
