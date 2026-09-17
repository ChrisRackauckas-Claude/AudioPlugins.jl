/* vst3_plugin_template.cpp -- a VST3 plugin around a per-sample C step function.
 *
 * The VST3 counterpart of clap_plugin_template.c, wrapping the same ABI:
 *
 *   @BASE@_out @BASE@_step(<inputs...>, @PARS@ *pars, @BASE@_mem *self);
 *   void       @BASE@_reset(@BASE@_mem *self);
 *
 * Not compilable as it stands: AudioPlugins.export_plugin renders it by
 * substituting every at-sign-delimited token from a PluginSpec, then
 * compiles the result as C++17 under -Wall -Wextra -Werror against the
 * Steinberg VST3 SDK (>= 3.8, MIT). The step function is compiled
 * separately, as C, and only has to satisfy the ABI declared in @HEADER@.
 *
 * One `@BASE@_mem` per channel persists across process() calls; that is
 * what makes two consecutive blocks equal one continuous run.
 *
 * A single-component effect (processor and controller in one object, no
 * editor), because the step function has no separable UI state. The one
 * thing to get right here and nowhere else in the exporter is that VST3
 * parameter values on the wire are NORMALISED to [0, 1] while the
 * descriptor -- and so the parameter struct the step function reads --
 * is in plain units. The controller owns that mapping: every parameter
 * is a RangeParameter over the descriptor's [min, max], and the
 * conversion below is EditController::normalizedParamToPlain, i.e. the
 * controller's own, so what the DSP sees can never drift from what the
 * host is told.
 */

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "base/source/fstreamer.h"

#include <cstring>

extern "C" {
#include "@HEADER@"
}

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

typedef @PARS@ ap_pars_t;
typedef @BASE@_mem ap_mem_t;
typedef @BASE@_out ap_out_t;

#define AP_CHANNELS  @CHANNELS@
#define AP_N_PARAMS  @N_PARAMS@
#define AP_LATENCY   @LATENCY@
#define AP_STATE_VERSION 1

/* ---------------------------------------------------------------- *
 * The parameter table, in the descriptor's plain units
 * ---------------------------------------------------------------- */

struct ap_param_info_t {
    ParamID      id;
    const TChar *name;
    double       min, max, def;
    int32        steps;        /* ParameterInfo::stepCount; 0 is continuous */
    int32        flags;
    bool         stepped;      /* the plain value is rounded to an integer */
};

/* One trailing sentinel so a plugin with no parameters still has an array. */
static const ap_param_info_t PARAM_INFO[AP_N_PARAMS + 1] = {
    @PARAM_INFO@
    { 0, STR16(""), 0.0, 0.0, 0.0, 0, 0, false },
};

static int param_index(ParamID id) {
    for (int i = 0; i < AP_N_PARAMS; i++)
        if (PARAM_INFO[i].id == id) return i;
    return -1;
}

static double round_half_away(double v) {
    return (double)(long long)(v + (v >= 0.0 ? 0.5 : -0.5));
}

/* ---------------------------------------------------------------- *
 * The plugin
 * ---------------------------------------------------------------- */

class ap_plugin_t : public SingleComponentEffect {
public:
    static FUnknown *createInstance(void *) { return (IAudioProcessor *)new ap_plugin_t; }

    tresult PLUGIN_API initialize(FUnknown *context) SMTG_OVERRIDE {
        tresult r = SingleComponentEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioInput(STR16("In"), @ARRANGEMENT@);
        addAudioOutput(STR16("Out"), @ARRANGEMENT@);
        for (int i = 0; i < AP_N_PARAMS; i++) {
            const ap_param_info_t *pi = &PARAM_INFO[i];
            parameters.addParameter(new RangeParameter(pi->name, pi->id, STR16(""),
                                                       pi->min, pi->max, pi->def,
                                                       pi->steps, pi->flags));
        }
        @CONSTANTS@
        for (int i = 0; i < AP_N_PARAMS; i++) apply_plain(i, PARAM_INFO[i].def);
        reset_state();
        return kResultOk;
    }

    /* One arrangement, the descriptor's, or any narrower one: the step
     * function is per-channel, so N channels are N independent states. */
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns,
                                          SpeakerArrangement *outputs,
                                          int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1) return kResultFalse;
        if (inputs[0] != outputs[0]) return kResultFalse;
        int32 nch = SpeakerArr::getChannelCount(inputs[0]);
        if (nch < 1 || nch > AP_CHANNELS) return kResultFalse;
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    /* The step function's own latency, reported and never introduced here:
     * a lookahead lives in the step, the wrapper only says how long it is. */
    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return (uint32)AP_LATENCY; }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
             ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) SMTG_OVERRIDE {
        if (!(setup.sampleRate > 0.0)) return kResultFalse;
        tresult r = SingleComponentEffect::setupProcessing(setup);
        if (r != kResultOk) return r;
        double sr = setup.sampleRate;
        @ON_ACTIVATE@
        reset_state();
        return kResultOk;
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (state) reset_state();
        return SingleComponentEffect::setActive(state);
    }

    tresult PLUGIN_API process(ProcessData &data) SMTG_OVERRIDE {
        /* Automation queues for the parameters this plugin has, with a
         * cursor each: the points are ordered by sample offset, so one
         * pass over the block applies every change where it belongs. */
        IParamValueQueue *queue[AP_N_PARAMS + 1];
        int index[AP_N_PARAMS + 1];
        int32 cursor[AP_N_PARAMS + 1];
        int n_queues = 0;
        if (data.inputParameterChanges) {
            int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n && n_queues < AP_N_PARAMS; i++) {
                IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
                if (!q || q->getPointCount() < 1) continue;
                int pi = param_index(q->getParameterId());
                if (pi < 0) continue;                /* an id we do not have */
                queue[n_queues] = q;
                index[n_queues] = pi;
                cursor[n_queues] = 0;
                n_queues++;
            }
        }

        if (data.numInputs >= 1 && data.numOutputs >= 1 && data.numSamples > 0) {
            AudioBusBuffers &in = data.inputs[0], &out = data.outputs[0];
            int32 nch = in.numChannels < out.numChannels ? in.numChannels : out.numChannels;
            if (nch > AP_CHANNELS) nch = AP_CHANNELS;
            bool f64 = data.symbolicSampleSize == kSample64;
            for (int32 i = 0; i < data.numSamples; i++) {
                for (int k = 0; k < n_queues; k++) apply_points(queue[k], index[k], cursor[k], i);
                for (int32 c = 0; c < nch; c++) {
                    double x = f64 ? in.channelBuffers64[c][i] : (double)in.channelBuffers32[c][i];
                    ap_out_t o = @BASE@_step(@STEP_ARGS@ &pars_, &mem_[c]);
                    @OUTPUT_READ@
                    if (f64) out.channelBuffers64[c][i] = y;
                    else     out.channelBuffers32[c][i] = (float)y;
                }
            }
            out.silenceFlags = 0;
        }

        /* Anything left over (a flush call, or a point past the end of the
         * block) applies now, and the controller is told where each
         * parameter ended up -- the value that took effect, clamped and
         * rounded, not the one that arrived -- so the two views agree. */
        for (int k = 0; k < n_queues; k++) {
            apply_points(queue[k], index[k], cursor[k], kMaxInt32);
            sync_controller(index[k]);
        }
        return kResultOk;
    }

    /* The parameter values, normalised, little-endian on the wire:
     *   "APST" | i32 version | i32 count | count x (i32 id, f64 value)
     * One state serves both interfaces: a single-component effect's
     * IComponent::setState and IEditController::setState are the same
     * method, and the DSP struct is derived from the parameters. */
    tresult PLUGIN_API setState(IBStream *state) SMTG_OVERRIDE {
        if (!state) return kResultFalse;
        IBStreamer s(state, kLittleEndian);
        char magic[4] = {0};
        int32 version = 0, count = 0;
        if (s.readRaw(magic, 4) != 4 || memcmp(magic, "APST", 4) != 0) return kResultFalse;
        if (!s.readInt32(version) || version != AP_STATE_VERSION) return kResultFalse;
        if (!s.readInt32(count) || count < 0) return kResultFalse;
        for (int32 k = 0; k < count; k++) {
            int32 id = 0;
            double v = 0.0;
            if (!s.readInt32(id) || !s.readDouble(v)) return kResultFalse;
            int i = param_index((ParamID)id);
            if (i < 0) continue;                     /* an id this build lacks is skipped */
            apply_normalized(i, v);
            sync_controller(i);
        }
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream *state) SMTG_OVERRIDE {
        if (!state) return kResultFalse;
        IBStreamer s(state, kLittleEndian);
        if (s.writeRaw("APST", 4) != 4) return kResultFalse;
        if (!s.writeInt32(AP_STATE_VERSION) || !s.writeInt32(AP_N_PARAMS)) return kResultFalse;
        for (int i = 0; i < AP_N_PARAMS; i++)
            if (!s.writeInt32((int32)PARAM_INFO[i].id) ||
                !s.writeDouble(getParamNormalized(PARAM_INFO[i].id)))
                return kResultFalse;
        return kResultOk;
    }

private:
    void reset_state() {
        for (int c = 0; c < AP_CHANNELS; c++) @BASE@_reset(&mem_[c]);
        for (int c = 0; c < AP_CHANNELS; c++) held_[c] = 0.0;
    }

    /* Every point at or before `upto` -- the controller's own mapping
     * from the normalised wire value to the descriptor's plain units. */
    void apply_points(IParamValueQueue *q, int pi, int32 &cursor, int32 upto) {
        int32 n = q->getPointCount();
        while (cursor < n) {
            int32 offset = 0;
            ParamValue v = 0.0;
            if (q->getPoint(cursor, offset, v) != kResultOk) { cursor++; continue; }
            if (offset > upto) return;
            apply_normalized(pi, v);
            cursor++;
        }
    }

    void apply_normalized(int pi, double normalized) {
        apply_plain(pi, normalizedParamToPlain(PARAM_INFO[pi].id, normalized));
    }

    void apply_plain(int pi, double v) {
        const ap_param_info_t *info = &PARAM_INFO[pi];
        if (v < info->min) v = info->min;
        if (v > info->max) v = info->max;
        if (info->stepped) v = round_half_away(v);
        plain_[pi] = v;
        switch (pi) {
        @PARAM_APPLY@
        default: break;
        }
    }

    void sync_controller(int pi) {
        ParamID id = PARAM_INFO[pi].id;
        setParamNormalized(id, plainParamToNormalized(id, plain_[pi]));
    }

    ap_pars_t pars_ {};
    ap_mem_t  mem_[AP_CHANNELS] {};
    double    held_[AP_CHANNELS] {};       /* last present output, for a sub-clock output */
    double    plain_[AP_N_PARAMS + 1] {};  /* what each parameter actually took */
};

} // namespace

/* ---------------------------------------------------------------- *
 * Factory and entry
 *
 * The class id is derived from the descriptor's plugin id (see
 * AudioPlugins._vst3_class_id): a VST3 class is named by a 128-bit UID
 * and the descriptor carries a reverse-DNS string, so the string is
 * hashed, reproducibly, into one.
 * ---------------------------------------------------------------- */

BEGIN_FACTORY_DEF(@VENDOR@, @URL@, "")

    DEF_CLASS2(INLINE_UID(@CID@),
               PClassInfo::kManyInstances,
               kVstAudioEffectClass,
               @NAME@,
               0,                          /* a single-component effect is not distributable */
               @SUBCATEGORY@,
               @VERSION@,
               kVstVersionString,
               ap_plugin_t::createInstance)

END_FACTORY

/* The module entry points (ModuleEntry / ModuleExit) come from the SDK's
 * per-platform main file, compiled alongside this one:
 *   public.sdk/source/main/linuxmain.cpp | macmain.cpp | dllmain.cpp */
