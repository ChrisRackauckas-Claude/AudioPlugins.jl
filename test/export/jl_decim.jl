# jl_decim.jl -- the decimator fixture in Julia: an output on a sub-clock,
# present every `divisor`-th sample and NaN in between.
struct JlDecimPars
    gain::Float64
    divisor::Int64
end

struct JlDecimMem
    n::Int64
end

struct JlDecimOut
    y::Float64
    has_y::Bool
end

Base.@ccallable function jl_decim_step(u::Float64, pars::Ptr{JlDecimPars}, self::Ptr{JlDecimMem})::JlDecimOut
    p = unsafe_load(pars)
    n = unsafe_load(self).n
    d = max(p.divisor, 1)
    present = n % d == 0
    unsafe_store!(self, JlDecimMem(n + 1))
    return JlDecimOut(present ? p.gain * u : NaN, present)
end

Base.@ccallable function jl_decim_reset(self::Ptr{JlDecimMem})::Cvoid
    unsafe_store!(self, JlDecimMem(0))
    return nothing
end
