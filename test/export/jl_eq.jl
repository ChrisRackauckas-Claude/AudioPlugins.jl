# jl_eq.jl -- the RBJ peaking equaliser fixture in Julia, coefficients
# recomputed every sample, state in a direct-form-I biquad.
struct JlEqPars
    fs::Float64
    f0::Float64
    q::Float64
    gain_db::Float64
    enabled::Bool
end

struct JlEqMem
    x1::Float64
    x2::Float64
    y1::Float64
    y2::Float64
end

struct JlEqOut
    y::Float64
end

Base.@ccallable function jl_eq_step(u::Float64, pars::Ptr{JlEqPars}, self::Ptr{JlEqMem})::JlEqOut
    p = unsafe_load(pars)
    p.enabled || return JlEqOut(u)
    m = unsafe_load(self)
    A = 10.0^(p.gain_db / 40.0)
    w0 = 2.0 * pi * p.f0 / p.fs
    alpha = sin(w0) / (2.0 * p.q)
    cw = cos(w0)
    b0, b1, b2 = 1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A
    a0, a1, a2 = 1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A
    y = (b0 / a0) * u + (b1 / a0) * m.x1 + (b2 / a0) * m.x2 - (a1 / a0) * m.y1 - (a2 / a0) * m.y2
    unsafe_store!(self, JlEqMem(u, m.x1, y, m.y1))
    return JlEqOut(y)
end

Base.@ccallable function jl_eq_reset(self::Ptr{JlEqMem})::Cvoid
    unsafe_store!(self, JlEqMem(0.0, 0.0, 0.0, 0.0))
    return nothing
end
