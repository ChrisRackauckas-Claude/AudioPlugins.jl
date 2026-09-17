/* probe_vst3_state.cpp -- the authored VST3 wrapper's state, at C++ level.
 *
 * The Julia VST3 host has no entry point for IComponent::setState /
 * getState, so the one part of the wrapper it cannot reach is tested
 * here: the plugin is linked into this program rather than loaded, its
 * factory called directly, and a preset round-tripped through a
 * MemoryStream.
 *
 * Built by test/export_vst3_tests.jl out of the same rendered wrapper
 * export_plugin compiles, so what is tested is the shipped template.
 *
 *   ./probe_vst3_state
 */
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

static int fails;
static void ck(bool ok, const char *what) {
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    IPluginFactory *factory = GetPluginFactory();
    ck(factory != nullptr, "the wrapper exports a factory");
    if (!factory) return 1;
    ck(factory->countClasses() == 1, "with exactly one class");

    PClassInfo ci = {};
    ck(factory->getClassInfo(0, &ci) == kResultOk, "whose class info reads");

    FUnknown *obj = nullptr;
    ck(factory->createInstance(ci.cid, FUnknown::iid, (void **)&obj) == kResultOk && obj,
       "the class instantiates");
    if (!obj) return 1;

    FUnknownPtr<IComponent> component(obj);
    FUnknownPtr<IEditController> controller(obj);
    ck(component != nullptr, "  it is an IComponent");
    ck(controller != nullptr, "  and an IEditController: one single component");
    if (!component || !controller) return 1;
    ck(component->initialize(nullptr) == kResultOk, "it initialises");

    int32 n = controller->getParameterCount();
    ck(n > 0, "it has parameters");
    ParameterInfo info = {};
    ck(controller->getParameterInfo(0, info) == kResultOk, "  the first reads back");
    ParamID id = info.id;

    /* A value that is neither the default nor an endpoint, so restoring it
     * means something. */
    const ParamValue saved = 0.75;
    ck(controller->setParamNormalized(id, saved) == kResultOk, "a value is set");
    ck(controller->getParamNormalized(id) == saved, "  and reads back");

    MemoryStream stream;
    ck(component->getState(&stream) == kResultOk, "getState writes a preset");
    ck(stream.getSize() > 0, "  which is not empty");
    ck(memcmp(stream.getData(), "APST", 4) == 0, "  and starts with the magic");

    ck(controller->setParamNormalized(id, 0.0) == kResultOk, "the value is clobbered");
    int64 pos = 0;
    ck(stream.seek(0, IBStream::kIBSeekSet, &pos) == kResultOk, "the preset rewinds");
    ck(component->setState(&stream) == kResultOk, "setState reads it back");
    ck(controller->getParamNormalized(id) == saved, "  and the value is the saved one");

    /* Someone else's preset is refused rather than misread. */
    MemoryStream foreign;
    int32 written = 0;
    foreign.write((void *)"NOPE\0\0\0\0", 8, &written);
    foreign.seek(0, IBStream::kIBSeekSet, &pos);
    ck(component->setState(&foreign) != kResultOk, "a foreign preset is refused");
    ck(controller->getParamNormalized(id) == saved, "  and changes nothing");
    ck(component->setState(nullptr) != kResultOk, "a null stream is refused");

    component->terminate();
    obj->release();

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PROBES PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
