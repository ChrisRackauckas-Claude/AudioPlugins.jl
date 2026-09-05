# jl_gain.jl -- the gain fixture as Julia @ccallables for juliac: the same
# ABI fx_gain.h declares, with the structs read off these signatures.
struct JlGainPars
    gain::Float64
    bypass::Bool
end

struct JlGainMem
    ticks::Int64
end

struct JlGainOut
    y::Float64
end

Base.@ccallable function jl_gain_step(u::Float64, clock1::Bool, pars::Ptr{JlGainPars},
                                      self::Ptr{JlGainMem})::JlGainOut
    p = unsafe_load(pars)
    if clock1
        unsafe_store!(self, JlGainMem(unsafe_load(self).ticks + 1))
    end
    return JlGainOut(p.bypass ? u : p.gain * u)
end

Base.@ccallable function jl_gain_reset(self::Ptr{JlGainMem})::Cvoid
    unsafe_store!(self, JlGainMem(0))
    return nothing
end
