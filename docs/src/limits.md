# Known limits

These are properties of the design as it stands, not bugs waiting on a fix in the next
patch release. Each is tracked as an issue.

## Plugins run in-process

Third-party binary code is loaded into the Julia process and called directly. There is no
sandbox: a plugin that segfaults takes the Julia process down with it, and a plugin that
corrupts memory corrupts yours.

Out-of-process hosting is the robust answer, and it is a much larger project — a
separate process, a shared-memory audio transport, and a protocol for the lifecycle and
the parameter events. In-process is fine for offline work, and offline work is what this
is for.

Tracked as [issue #7](https://github.com/SciML/AudioPlugins.jl/issues/7).

## No realtime discipline

CLAP asks a host to call `process()` from a realtime thread that never blocks, never
allocates and never takes a lock, and it asks the host to honour the same discipline. A
garbage-collected process driving a solver that may retry a step cannot promise any of
that: the call may come from anywhere, a step may be recomputed, and the surrounding
Julia process allocates and collects freely.

Offline this is harmless — nobody is listening as the output is produced, so a late block
is just a slow run. It stops being harmless the moment this is driven from a live capture
with a deadline, where a plugin that allocates or blocks inside `process()` produces
dropouts that look like a modelling error.

Tracked as [issue #8](https://github.com/SciML/AudioPlugins.jl/issues/8).

## Reported latency is surfaced, not compensated

[`clap_latency`](@ref) returns the latency the plugin declares, in samples. The host
reports that number and does nothing with it. Hosting a lookahead limiter therefore leaves
its output shifted by that many samples relative to the input.

Compensating it means delaying the dry path by the same amount, which needs a delay line
the host does not have. A caller that cares must read the number and align downstream
itself. The `ap.lookahead` test plugin exists to keep this honest: its reported latency is
real, and the tests assert that it is surfaced rather than silently absorbed.

Tracked as [issue #9](https://github.com/SciML/AudioPlugins.jl/issues/9).

## Contiguity is unchecked

The host knows the block size and the sample rate but cannot see the caller's clock, so it
cannot check that it is being fed every sample exactly once. A plugin fed a
non-contiguous stream returns a perfectly valid-looking result that is not continuous
audio. The token discipline described in [Tokens, and why the API has them](@ref)
catches a *stale* block, not a *skipped* one.
