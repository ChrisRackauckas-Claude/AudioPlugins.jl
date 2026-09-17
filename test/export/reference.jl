# The arithmetic the fixtures under this directory are supposed to do,
# written independently of them: what a plugin built out of fx_gain.c,
# fx_eq.c or fx_decim.c must reproduce, whatever format it was built in.
# Included by export_tests.jl (CLAP) and export_vst3_tests.jl (VST3), so
# that the two suites check one plugin against one reference.

# The reference recursion, written the way fx_eq.c writes it so the only
# difference is the host's float32 sample storage.
function rbj_peaking(x, fs, f0, q, gain_db)
    A = 10.0^(gain_db / 40)
    w0 = 2pi * f0 / fs
    alpha = sin(w0) / (2q)
    cw = cos(w0)
    b0, b1, b2 = 1 + alpha * A, -2cw, 1 - alpha * A
    a0, a1, a2 = 1 + alpha / A, -2cw, 1 - alpha / A
    x1 = x2 = y1 = y2 = 0.0
    y = similar(x)
    for i in eachindex(x)
        y[i] = (b0 / a0) * x[i] + (b1 / a0) * x1 + (b2 / a0) * x2 - (a1 / a0) * y1 - (a2 / a0) * y2
        x2, x1 = x1, x[i]
        y2, y1 = y1, y[i]
    end
    return y
end

# A held sample-and-hold: y[i] = gain * x[k] for the latest k <= i with k % d == 0.
function decimate_hold(x, d, gain)
    y = similar(x)
    held = 0.0
    for (i, v) in enumerate(x)
        (i - 1) % d == 0 && (held = gain * v)
        y[i] = held
    end
    return y
end

# Inputs that are exactly representable as float32, so that an exact
# comparison against the host's float32 buffers means what it says.
f32(x) = Float64.(Float32.(x))
signal(n) = f32([0.4sin(2pi * 0.013i) + 0.3sin(2pi * 0.171i) for i in 0:(n - 1)])
