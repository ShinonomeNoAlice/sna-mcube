#include "VstHost.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <chrono>
#include "toml.hpp"

using namespace Steinberg;
using namespace Steinberg::Vst;

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/vstpresetfile.h"
#include "pluginterfaces/base/istringresult.h"

namespace Steinberg {
    DEF_CLASS_IID (IString)
    DEF_CLASS_IID (IStringResult)
}

// Global static VST module cache to keep VST DLLs loaded in memory,
// avoiding expensive disk access/unloading on every track change.
static std::mutex gModuleCacheMutex;
static std::map<std::string, VST3::Hosting::Module::Ptr> gModuleCache;


// Helper to convert std::string to std::wstring
static std::wstring utf8ToUtf16(const std::string& utf8str) {
    if (utf8str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, &utf8str[0], (int)utf8str.size(), NULL, 0);
    std::wstring utf16str(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &utf8str[0], (int)utf8str.size(), &utf16str[0], size);
    return utf16str;
}

static void LogDebug(const std::string& msg) {
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        std::string logPath = std::string(appdata) + "\\musikcube\\wasapiexclusive_debug.txt";
        free(appdata);
        std::ofstream log(logPath, std::ios::app);
        if (log) {
            log << "[VST] " << msg << std::endl;
        }
    }
}

// Simple in-memory implementation of IBStream for VST3 component-to-controller state sync
class MemoryStreamHelper : public Steinberg::IBStream {
private:
    std::vector<char> data;
    int64_t position = 0;
    uint32_t refCount = 1;
public:
    virtual ~MemoryStreamHelper() {}

    // Implement FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0 ||
            memcmp(_iid, Steinberg::IBStream::iid, 16) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        int32 available = (int32)(data.size() - position);
        if (numBytes > available) numBytes = available;
        if (numBytes > 0) {
            memcpy(buffer, data.data() + position, numBytes);
            position += numBytes;
        }
        if (numBytesRead) *numBytesRead = numBytes;
        return kResultTrue;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (position + numBytes > (int64_t)data.size()) {
            data.resize((size_t)(position + numBytes));
        }
        if (numBytes > 0) {
            memcpy(data.data() + position, buffer, numBytes);
            position += numBytes;
        }
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultTrue;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        if (mode == Steinberg::IBStream::kIBSeekSet) position = pos;
        else if (mode == Steinberg::IBStream::kIBSeekCur) position += pos;
        else if (mode == Steinberg::IBStream::kIBSeekEnd) position = data.size() + pos;
        if (position < 0) position = 0;
        if (position > (int64_t)data.size()) position = data.size();
        if (result) *result = position;
        return kResultTrue;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos) *pos = position;
        return kResultTrue;
    }
    const std::vector<char>& getData() const { return data; }
};

class ComponentHandlerHelper : public Steinberg::Vst::IComponentHandler {
private:
    uint32_t refCount = 1;
    VstPlugin* plugin;
public:
    ComponentHandlerHelper(VstPlugin* p) : plugin(p) {}
    virtual ~ComponentHandlerHelper() {}

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0 ||
            memcmp(_iid, Steinberg::Vst::IComponentHandler::iid.toTUID(), 16) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    tresult PLUGIN_API beginEdit(ParamID id) override {
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID id) override {
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        return kResultOk;
    }
};

class ParamValueQueueHelper : public Steinberg::Vst::IParamValueQueue {
private:
    ParamID id;
    ParamValue value;
    uint32_t refCount = 1;
public:
    ParamValueQueueHelper(ParamID id, ParamValue value) : id(id), value(value) {}
    virtual ~ParamValueQueueHelper() {}
    
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0 ||
            memcmp(_iid, Steinberg::Vst::IParamValueQueue::iid.toTUID(), 16) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& val) override {
        if (index == 0) {
            sampleOffset = 0;
            val = value;
            return kResultTrue;
        }
        return kResultFalse;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue val, int32& index) override {
        value = val;
        index = 0;
        return kResultTrue;
    }
};

class ParameterChangesHelper : public Steinberg::Vst::IParameterChanges {
private:
    std::vector<IPtr<ParamValueQueueHelper>> queues;
    uint32_t refCount = 1;
public:
    ParameterChangesHelper() {}
    virtual ~ParameterChangesHelper() {}
    
    void addChange(ParamID id, ParamValue value) {
        queues.push_back(owned(new ParamValueQueueHelper(id, value)));
    }

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0 ||
            memcmp(_iid, Steinberg::Vst::IParameterChanges::iid.toTUID(), 16) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    int32 PLUGIN_API getParameterCount() override { return (int32)queues.size(); }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index >= 0 && index < (int32)queues.size()) {
            return queues[index].get();
        }
        return nullptr;
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        for (int i = 0; i < (int)queues.size(); ++i) {
            if (queues[i]->getParameterId() == id) {
                index = i;
                return queues[i].get();
            }
        }
        index = (int32)queues.size();
        auto q = owned(new ParamValueQueueHelper(id, 0.0));
        queues.push_back(q);
        return q.get();
    }
};

class PlugFrameHelper : public Steinberg::IPlugFrame {
private:
    HWND parentHwnd;
    uint32_t refCount = 1;
public:
    PlugFrameHelper(HWND hwnd) : parentHwnd(hwnd) {}
    virtual ~PlugFrameHelper() {}

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0 ||
            memcmp(_iid, Steinberg::IPlugFrame::iid.toTUID(), 16) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (!newSize) return kInvalidArgument;
        RECT rw = { 0, 0, newSize->right - newSize->left, newSize->bottom - newSize->top };
        AdjustWindowRect(&rw, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(parentHwnd, nullptr, 0, 0, rw.right - rw.left, rw.bottom - rw.top, SWP_NOMOVE | SWP_NOZORDER);
        return kResultOk;
    }
};

// ---------------------------------------------------------
// VstPlugin
// ---------------------------------------------------------

VstPlugin::VstPlugin(const std::string& path, const std::string& presetPath, bool showUi, int orderIndex, const std::string& customTitle, VstChain* chain)
    : dllPath(path), presetPath(presetPath), showUiDesired(showUi), orderIndex(orderIndex), customTitle(customTitle), chain(chain) {
    LogDebug("VstPlugin constructor for path: " + path + ", orderIndex: " + std::to_string(orderIndex) + ", customTitle: " + customTitle);
}

VstPlugin::~VstPlugin() {
    LogDebug("VstPlugin destructor");
    Unload();
}

tresult PLUGIN_API ComponentHandlerHelper::performEdit(ParamID id, ParamValue value) {
    if (plugin) {
        plugin->OnParameterEdit(id, value);
    }
    return kResultOk;
}

void VstPlugin::OnParameterEdit(ParamID id, ParamValue value) {
    std::lock_guard<std::mutex> lock(paramMutex);
    for (auto& change : pendingParamChanges) {
        if (change.id == id) {
            change.value = value;
            return;
        }
    }
    pendingParamChanges.push_back({ id, value });
}

bool VstPlugin::Load() {
    LogDebug("VstPlugin::Load() start: " + dllPath);
    std::string errorStr;
    // Check cache first to keep DLL loaded and prevent heavy disk I/O on track changes
    {
        std::lock_guard<std::mutex> lock(gModuleCacheMutex);
        auto it = gModuleCache.find(dllPath);
        if (it != gModuleCache.end()) {
            module = it->second;
            LogDebug("VST Module retrieved from cache: " + dllPath);
        } else {
            module = VST3::Hosting::Module::create(dllPath, errorStr);
            if (module) {
                gModuleCache[dllPath] = module;
                LogDebug("VST Module loaded and cached: " + dllPath);
            }
        }
    }
    
    if (!module) {
        LogDebug("Failed to load VST3 module: " + errorStr);
        return false;
    }
    LogDebug("VST3 module loaded successfully");

    auto classInfos = module->getFactory().classInfos();
    bool found = false;
    for (const auto& info : classInfos) {
        LogDebug("Class category found: " + info.category() + ", name: " + info.name());
        if (info.category() == kVstAudioEffectClass) {
            this->effectId = FUID::fromTUID(info.ID().data());
            vstName = info.name();
            found = true;
            break;
        }
    }

    if (!found) {
        LogDebug("No audio effect class found in VST3 module");
        return false;
    }

    component = module->getFactory().createInstance<IComponent>(VST3::UID(this->effectId.toTUID()));
    if (!component) {
        LogDebug("Failed to create IComponent instance");
        return false;
    }
    LogDebug("IComponent instance created");

    if (component->queryInterface(IAudioProcessor::iid, (void**)&processor) != kResultTrue) {
        processor = nullptr;
        LogDebug("Failed to query IAudioProcessor");
    } else {
        LogDebug("IAudioProcessor queried successfully");
    }

    TUID controllerClassId;
    memset(controllerClassId, 0, sizeof(TUID));
    if (component->getControllerClassId(controllerClassId) == kResultTrue) {
        VST3::UID controllerId(controllerClassId);
        controller = module->getFactory().createInstance<IEditController>(controllerId);
    }

    if (!controller) {
        if (component->queryInterface(IEditController::iid, (void**)&controller) != kResultTrue) {
            controller = nullptr;
            LogDebug("Failed to query IEditController (optional)");
        } else {
            LogDebug("IEditController queried successfully from component");
        }
    } else {
        LogDebug("IEditController created successfully via class ID");
    }

    // Initialize component
    LogDebug("Initializing component...");
    IPtr<HostApplication> hostContext = owned(new HostApplication());
    if (component->initialize(hostContext) != kResultTrue) {
        LogDebug("Component initialization failed");
        return false;
    }
    LogDebug("Component initialized");
    
    // Activate all audio input and output buses (required for signal flow in VST3)
    int32 numInputBuses = component->getBusCount(MediaTypes::kAudio, BusDirections::kInput);
    for (int32 i = 0; i < numInputBuses; ++i) {
        component->activateBus(MediaTypes::kAudio, BusDirections::kInput, i, true);
    }
    int32 numOutputBuses = component->getBusCount(MediaTypes::kAudio, BusDirections::kOutput);
    for (int32 i = 0; i < numOutputBuses; ++i) {
        component->activateBus(MediaTypes::kAudio, BusDirections::kOutput, i, true);
    }
    LogDebug("Buses activated");
    
    if (controller) {
        LogDebug("Initializing controller...");
        controller->initialize(hostContext);
        
        IPtr<ComponentHandlerHelper> handler = owned(new ComponentHandlerHelper(this));
        controller->setComponentHandler(handler);
        LogDebug("Component handler set on controller");

        // Sync component state to controller
        IPtr<MemoryStreamHelper> stateStream = owned(new MemoryStreamHelper());
        if (component->getState(stateStream) == kResultTrue) {
            int64_t resultPos = 0;
            stateStream->seek(0, Steinberg::IBStream::kIBSeekSet, &resultPos);
            controller->setComponentState(stateStream);
            LogDebug("Synced component state to controller");
        } else {
            controller->setComponentState(nullptr);
        }

        // Establish connection between DSP (component) and UI (controller)
        IPtr<IConnectionPoint> componentConnection;
        IPtr<IConnectionPoint> controllerConnection;
        if (component->queryInterface(IConnectionPoint::iid, (void**)&componentConnection) == kResultTrue &&
            controller->queryInterface(IConnectionPoint::iid, (void**)&controllerConnection) == kResultTrue) {
            componentConnection->connect(controllerConnection);
            controllerConnection->connect(componentConnection);
            LogDebug("DSP and UI ConnectionPoints connected");
        }

        LogDebug("Controller initialized");
    }

    SetupProcessing();
    
    if (presetPath.empty() && chain && autoloadEnabled) {
        std::filesystem::path tomlPath(chain->GetConfigPath());
        std::filesystem::path tomlDir = tomlPath.parent_path();
        std::string filename = "plugin_" + std::to_string(orderIndex) + ".vstpreset";
        presetPath = (tomlDir / filename).string();
        LogDebug("Auto-mapped empty preset path to: " + presetPath);
    }
    
    if (!presetPath.empty() && std::filesystem::exists(presetPath)) {
        LoadPreset(presetPath);
    }
    
    CheckUiState(showUiDesired);

    LogDebug("VstPlugin::Load() succeeded");
    return true;
}

void VstPlugin::Unload() {
    LogDebug("VstPlugin::Unload() start");
    if (!presetPath.empty() && autoloadEnabled) {
        SavePreset(presetPath);
    }
    CheckUiState(false);

    if (component && controller) {
        IPtr<IConnectionPoint> componentConnection;
        IPtr<IConnectionPoint> controllerConnection;
        if (component->queryInterface(IConnectionPoint::iid, (void**)&componentConnection) == kResultTrue &&
            controller->queryInterface(IConnectionPoint::iid, (void**)&controllerConnection) == kResultTrue) {
            componentConnection->disconnect(controllerConnection);
            controllerConnection->disconnect(componentConnection);
            LogDebug("DSP and UI ConnectionPoints disconnected");
        }
    }

    if (processor) {
        LogDebug("Deactivating processor...");
        processor->setProcessing(false);
        processor = nullptr;
    }
    if (component) {
        LogDebug("Terminating component...");
        component->setActive(false);
        component->terminate();
        component = nullptr;
    }
    if (controller) {
        LogDebug("Terminating controller...");
        controller->terminate();
        controller = nullptr;
    }
    module = nullptr;
    LogDebug("VstPlugin::Unload() finished");
}

void VstPlugin::SetupProcessing() {
    LogDebug("VstPlugin::SetupProcessing() start");
    if (!processor || !component) {
        LogDebug("No processor or component present, skipping SetupProcessing");
        return;
    }
    
    this->currentSampleRate = 44100.0;
    this->currentBlockSize = 16384;
    
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = this->currentBlockSize;
    setup.sampleRate = this->currentSampleRate;
    
    LogDebug("Setting setupProcessing on processor...");
    processor->setupProcessing(setup);
    LogDebug("Activating component...");
    component->setActive(true);
    LogDebug("Setting processing true on processor...");
    processor->setProcessing(true);
    LogDebug("VstPlugin::SetupProcessing() finished");
}

void VstPlugin::SetSampleRateAndBlockSize(double sampleRate, int blockSize) {
    if (!processor || !component) return;
    
    if (sampleRate == this->currentSampleRate && blockSize <= this->currentBlockSize) {
        return;
    }
    
    LogDebug("VstPlugin::SetSampleRateAndBlockSize(" + std::to_string(sampleRate) + ", " + std::to_string(blockSize) + ") start");
    
    // Reset sample timing counter and anchor timestamp ONLY on sample rate change.
    // If only block size expanded, preserve timeline continuity so visualizers don't jump back to 0.
    if (sampleRate != this->currentSampleRate) {
        totalSamplesProcessed = 0;
        streamStartSystemTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        this->currentSampleRate = sampleRate;
    }

    processor->setProcessing(false);
    component->setActive(false);
    
    int neededBlock = blockSize * 2;
    if (neededBlock > this->currentBlockSize) this->currentBlockSize = neededBlock;
    if (this->currentBlockSize < 16384) this->currentBlockSize = 16384;
    
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = this->currentBlockSize;
    setup.sampleRate = this->currentSampleRate;
    
    processor->setupProcessing(setup);
    component->setActive(true);
    processor->setProcessing(true);
    LogDebug("VstPlugin::SetSampleRateAndBlockSize() finished");
}

void VstPlugin::SavePreset(const std::string& path) {
    LogDebug("VstPlugin::SavePreset(" + path + ") start");
    if (!component) return;
    
    IPtr<MemoryStreamHelper> stream = owned(new MemoryStreamHelper());
    if (Steinberg::Vst::PresetFile::savePreset(stream, effectId, component, controller)) {
        std::ofstream file(path, std::ios::binary);
        if (file) {
            file.write(stream->getData().data(), stream->getData().size());
            file.close();
            LogDebug("Preset saved successfully to: " + path);
        } else {
            LogDebug("Failed to open preset file for writing: " + path);
        }
    } else {
        LogDebug("Failed to save preset to stream");
    }
}

void VstPlugin::LoadPreset(const std::string& path) {
    LogDebug("VstPlugin::LoadPreset(" + path + ") start");
    if (!component) return;
    
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LogDebug("Failed to open preset file for reading: " + path);
        return;
    }
    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    IPtr<MemoryStreamHelper> stream = owned(new MemoryStreamHelper());
    if (buffer.size() > 0) {
        int32_t written = 0;
        stream->write(buffer.data(), (int32_t)buffer.size(), &written);
    }
    int64_t pos = 0;
    stream->seek(0, Steinberg::IBStream::kIBSeekSet, &pos);

    if (Steinberg::Vst::PresetFile::loadPreset(stream, effectId, component, controller)) {
        loadedPresetPath = path;
        LogDebug("Preset loaded successfully from: " + path);
    } else {
        LogDebug("Failed to load preset from: " + path);
    }
}

void VstPlugin::Process(float** inputs, float** outputs, int numSamples, int numChannels) {
    if (!processor) return;
    
    // Construct a stable and monotonic ProcessContext to feed visualizers/meters/clocks
    // with reliable timing info, preventing visual stutter or timing anomalies.
    ProcessContext context;
    memset(&context, 0, sizeof(context));
    context.sampleRate = currentSampleRate;
    context.projectTimeSamples = totalSamplesProcessed;
    
    // Calculate systemTime deterministically based on sample count to eliminate WASAPI buffer scheduling jitter
    if (streamStartSystemTime == 0) {
        streamStartSystemTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    int64_t elapsedNs = (currentSampleRate > 0) 
        ? (int64_t)((double)totalSamplesProcessed * 1000000000.0 / currentSampleRate)
        : 0;
    context.systemTime = streamStartSystemTime + elapsedNs;
    
    context.state = ProcessContext::kPlaying | ProcessContext::kSystemTimeValid | ProcessContext::kContTimeValid | ProcessContext::kProjectTimeMusicValid;
    context.continousTimeSamples = totalSamplesProcessed;
    
    // Provide default friendly musical info (120 BPM, 4/4 time signature)
    context.tempo = 120.0;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;
    context.projectTimeMusic = (currentSampleRate > 0)
        ? ((double)totalSamplesProcessed / currentSampleRate * (context.tempo / 60.0))
        : 0.0;
    context.barPositionMusic = std::floor(context.projectTimeMusic / 4.0) * 4.0;
    context.state |= ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid | ProcessContext::kBarPositionValid;

    // Monotonically advance the audio sample counter
    totalSamplesProcessed += numSamples;
    
    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.numSamples = numSamples;
    data.processContext = &context;
    
    AudioBusBuffers inBus;
    inBus.numChannels = numChannels;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inputs;
    
    AudioBusBuffers outBus;
    outBus.numChannels = numChannels;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outputs;
    
    data.inputs = &inBus;
    data.outputs = &outBus;
    
    IPtr<ParameterChangesHelper> paramChanges = nullptr;
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        if (!pendingParamChanges.empty()) {
            paramChanges = owned(new ParameterChangesHelper());
            for (const auto& change : pendingParamChanges) {
                paramChanges->addChange(change.id, change.value);
            }
            pendingParamChanges.clear();
        }
    }
    data.inputParameterChanges = paramChanges ? paramChanges.get() : nullptr;
    
    // Allocate and assign output parameter changes to collect meters/LED levels
    IPtr<ParameterChangesHelper> outParamChanges = owned(new ParameterChangesHelper());
    data.outputParameterChanges = outParamChanges.get();
    
    processor->process(data);
    
    // Queue output parameter changes to UI thread safely without blocking audio thread
    int32 numOutChanges = outParamChanges->getParameterCount();
    if (numOutChanges > 0) {
        std::unique_lock<std::mutex> lock(outputParamMutex, std::defer_lock);
        if (lock.try_lock()) {
            for (int32 i = 0; i < numOutChanges; ++i) {
                if (auto* queue = outParamChanges->getParameterData(i)) {
                    int32 numPoints = queue->getPointCount();
                    if (numPoints > 0) {
                        int32 sampleOffset = 0;
                        ParamValue val = 0.0;
                        if (queue->getPoint(numPoints - 1, sampleOffset, val) == kResultTrue) {
                            latestOutputParams[queue->getParameterId()] = val;
                        }
                    }
                }
            }
        }
    }
}

void VstPlugin::CheckUiState(bool showUi) {
    LogDebug("VstPlugin::CheckUiState(" + std::to_string(showUi) + ") start");
    if (showUi && !hwnd && controller) {
        LogDebug("Attempting to obtain IPlugView on UI thread...");
        Steinberg::IPlugView* rawView = controller->createView(Steinberg::Vst::ViewType::kEditor);
        if (rawView) {
            view = IPtr<Steinberg::IPlugView>(rawView, false);
            LogDebug("IPlugView created via createView() on UI thread");
        } else {
            LogDebug("createView() failed on UI thread, falling back to queryInterface...");
            Steinberg::IPlugView* queriedView = nullptr;
            if (controller->queryInterface(Steinberg::IPlugView::iid, (void**)&queriedView) == kResultTrue) {
                view = IPtr<Steinberg::IPlugView>(queriedView, false);
                LogDebug("IPlugView obtained via queryInterface on UI thread");
            }
        }

        if (!view) {
            LogDebug("IPlugView not supported by this controller.");
            return;
        }

        // Use a unique class name per-instance to prevent conflicts when hosting multiple plugins
        std::wstring className = L"VstHostWindow_" + std::to_wstring((uintptr_t)this);

        WNDCLASS wc = { 0 };
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = className.c_str();
        RegisterClass(&wc);

        std::wstring wTitle;
        if (!customTitle.empty()) {
            wTitle = utf8ToUtf16(customTitle);
        } else {
            char indexStr[16];
            sprintf(indexStr, "%02d", orderIndex);
            std::string autoTitle = std::string(indexStr) + " - " + (vstName.empty() ? "VST3 Plugin" : vstName);
            wTitle = utf8ToUtf16(autoTitle);
        }

        hwnd = CreateWindowEx(
            0, wc.lpszClassName, wTitle.c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
            nullptr, nullptr, wc.hInstance, this);
            
        LogDebug("Window created, attaching view...");
        if (view) {
            // Create and set the plug frame helper before attaching so VSTGUI can query host properties
            Steinberg::IPtr<Steinberg::IPlugFrame> plugFrame = Steinberg::owned(new PlugFrameHelper(hwnd));
            view->setFrame(plugFrame.get());

            // 1. Attach the view first. This loads the XML, parses sizes, and sets up VSTGUI.
            view->attached((void*)hwnd, Steinberg::kPlatformTypeHWND);
            
            // 2. Query the size now that VSTGUI is initialized and knows its size.
            Steinberg::ViewRect rect;
            tresult sizeResult = view->getSize(&rect);
            LogDebug("view->getSize() result: " + std::to_string(sizeResult) + 
                     ", rect: L=" + std::to_string(rect.left) + 
                     ", T=" + std::to_string(rect.top) + 
                     ", R=" + std::to_string(rect.right) + 
                     ", B=" + std::to_string(rect.bottom));
                     
            if (sizeResult == kResultTrue && (rect.right - rect.left > 0) && (rect.bottom - rect.top > 0)) {
                RECT rw = { 0, 0, rect.right - rect.left, rect.bottom - rect.top };
                AdjustWindowRect(&rw, WS_OVERLAPPEDWINDOW, FALSE);
                LogDebug("Resizing window to: " + std::to_string(rw.right - rw.left) + "x" + std::to_string(rw.bottom - rw.top));
                SetWindowPos(hwnd, nullptr, 0, 0, rw.right - rw.left, rw.bottom - rw.top, SWP_NOMOVE | SWP_NOZORDER);
            } else {
                LogDebug("getSize returned invalid/zero size, applying 920x270 fallback size");
                RECT rw = { 0, 0, 920, 270 };
                AdjustWindowRect(&rw, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(hwnd, nullptr, 0, 0, rw.right - rw.left, rw.bottom - rw.top, SWP_NOMOVE | SWP_NOZORDER);
            }
            
            // 3. Enable WM_SIZE and WM_SIZING forwarding now that initial sizing is complete.
            viewAttached = true;
            LogDebug("View attached");
        }

        // Trigger WM_TIMER every 15ms (~66 FPS) to update VU meters and correlation indicators
        SetTimer(hwnd, 1, 15, nullptr);

        ShowWindow(hwnd, SW_SHOW);
    } else if (!showUi && hwnd) {
        LogDebug("Closing UI window...");
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
}

LRESULT CALLBACK VstPlugin::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    
    if (uMsg == WM_SIZING) {
        VstPlugin* plugin = (VstPlugin*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (plugin && plugin->view && plugin->viewAttached) {
            if (plugin->inResize) return TRUE;
            plugin->inResize = true;
            LPRECT prc = (LPRECT)lParam;
            RECT rcBorders = { 0, 0, 0, 0 };
            AdjustWindowRect(&rcBorders, WS_OVERLAPPEDWINDOW, FALSE);
            int clientWidth = (prc->right - prc->left) - (rcBorders.right - rcBorders.left);
            int clientHeight = (prc->bottom - prc->top) - (rcBorders.bottom - rcBorders.top);
            
            Steinberg::ViewRect rect(0, 0, clientWidth, clientHeight);
            if (plugin->view->checkSizeConstraint(&rect) == kResultTrue) {
                RECT rw = { 0, 0, rect.right - rect.left, rect.bottom - rect.top };
                AdjustWindowRect(&rw, WS_OVERLAPPEDWINDOW, FALSE);
                
                int dragWidth = rw.right - rw.left;
                int dragHeight = rw.bottom - rw.top;
                
                switch (wParam) {
                    case WMSZ_LEFT:
                    case WMSZ_TOPLEFT:
                    case WMSZ_BOTTOMLEFT:
                        prc->left = prc->right - dragWidth;
                        break;
                    case WMSZ_RIGHT:
                    case WMSZ_TOPRIGHT:
                    case WMSZ_BOTTOMRIGHT:
                    default:
                        prc->right = prc->left + dragWidth;
                        break;
                }
                switch (wParam) {
                    case WMSZ_TOP:
                    case WMSZ_TOPLEFT:
                    case WMSZ_TOPRIGHT:
                        prc->top = prc->bottom - dragHeight;
                        break;
                    case WMSZ_BOTTOM:
                    case WMSZ_BOTTOMLEFT:
                    case WMSZ_BOTTOMRIGHT:
                    default:
                        prc->bottom = prc->top + dragHeight;
                        break;
                }
                plugin->inResize = false;
                return TRUE;
            }
            plugin->inResize = false;
        }
    }
    
    if (uMsg == WM_SIZE) {
        VstPlugin* plugin = (VstPlugin*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (plugin && plugin->view && plugin->viewAttached) {
            if (!plugin->inResize) {
                plugin->inResize = true;
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                if (width > 0 && height > 0) {
                    Steinberg::ViewRect rect(0, 0, width, height);
                    plugin->view->onSize(&rect);
                }
                plugin->inResize = false;
            }
        }
    }

    if (uMsg == WM_TIMER) {
        VstPlugin* plugin = (VstPlugin*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (plugin && plugin->controller) {
            std::map<Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue> changes;
            {
                std::unique_lock<std::mutex> lock(plugin->outputParamMutex, std::defer_lock);
                if (lock.try_lock()) {
                    changes = plugin->latestOutputParams;
                    plugin->latestOutputParams.clear();
                }
            }
            for (const auto& change : changes) {
                plugin->controller->setParamNormalized(change.first, change.second);
            }
        }
        return 0;
    }

    if (uMsg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (uMsg == WM_DESTROY) {
        VstPlugin* plugin = (VstPlugin*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (plugin) {
            LogDebug("WM_DESTROY received. Removing view...");
            plugin->viewAttached = false;
            if (plugin->view) {
                plugin->view->setFrame(nullptr);
                plugin->view->removed();
                plugin->view = nullptr; // Release the COM reference
            }
            
            KillTimer(hwnd, 1);
            plugin->hwnd = nullptr;
            
            std::wstring className = L"VstHostWindow_" + std::to_wstring((uintptr_t)plugin);
            UnregisterClass(className.c_str(), GetModuleHandle(nullptr));
        }
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}


// ---------------------------------------------------------
// VstChain
// ---------------------------------------------------------

VstChain::VstChain(const std::string& tomlConfigPath) : configPath(tomlConfigPath) {
    LogDebug("VstChain constructor for path: " + tomlConfigPath);
    hostThreadRunning = true;
    hostThread = std::thread(&VstChain::WatchThread, this);
    
    // Wait for hostThreadId to be populated by the newly spawned thread
    while (hostThreadId == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    // Trigger the initial TOML config load on the STA thread
    PostThreadMessageW(hostThreadId, WM_USER + 101, 0, 0);
}

VstChain::~VstChain() {
    LogDebug("VstChain destructor start");
    hostThreadRunning = false;
    if (hostThreadId != 0) {
        PostThreadMessageW(hostThreadId, WM_QUIT, 0, 0);
    }
    if (hostThread.joinable()) {
        hostThread.join();
    }
    LogDebug("VstChain destructor finish");
}

void VstChain::SavePluginState(VstPlugin* plugin) {
    LogDebug("VstChain::SavePluginState() start");
    if (!plugin) return;
    
    // Determine the preset path
    std::string presetPath = plugin->GetPresetPath();
    if (presetPath.empty()) {
        // Generate a default path next to the TOML file
        std::filesystem::path tomlPath(configPath);
        std::filesystem::path tomlDir = tomlPath.parent_path();
        std::string filename = "plugin_" + std::to_string(plugin->GetOrderIndex()) + ".vstpreset";
        presetPath = (tomlDir / filename).string();
        plugin->SetPresetPath(presetPath);
    }
    
    // Save preset to file
    plugin->SavePreset(presetPath);
    plugin->SetLoadedPresetPath(presetPath);
    
    // Update the TOML config file
    try {
        LogDebug("Reading TOML to update preset path...");
        toml::table config = toml::parse_file(configPath);
        auto chain = config["chain"].as_array();
        if (chain && (plugin->GetOrderIndex() - 1) < (int)chain->size()) {
            auto& node = (*chain)[plugin->GetOrderIndex() - 1];
            auto& tbl = *node.as_table();
            tbl.insert_or_assign("preset", presetPath);
            
            std::ofstream ofs(configPath);
            if (ofs) {
                ofs << config;
                ofs.close();
                LogDebug("TOML config updated with new preset path: " + presetPath);
            }
        }
    } catch (const std::exception& e) {
        LogDebug("Failed to update TOML config: " + std::string(e.what()));
    }
}

void VstChain::ReloadConfig() {
    LogDebug("VstChain::ReloadConfig() start");
    std::lock_guard<std::mutex> lock(chainMutex);

    if (configPath.empty()) {
        LogDebug("Config path is empty, clearing plugins");
        plugins.clear();
        return;
    }

    if (!std::filesystem::exists(configPath)) {
        LogDebug("Config file does not exist, clearing plugins: " + configPath);
        plugins.clear();
        return;
    }

    try {
        LogDebug("Parsing TOML file...");
        toml::table config = toml::parse_file(configPath);
        auto chain = config["chain"].as_array();
        
        if (chain) {
            size_t newSize = chain->size();
            LogDebug("Found chain array, count: " + std::to_string(newSize));
            
            std::vector<std::unique_ptr<VstPlugin>> newPlugins;
            
            for (size_t i = 0; i < newSize; ++i) {
                auto& node = (*chain)[i];
                auto& tbl = *node.as_table();
                std::string path = tbl["path"].value_or<std::string>("");
                std::string preset = tbl["preset"].value_or<std::string>("");
                bool showUi = tbl["show_ui"].value_or<bool>(false);
                bool bypass = tbl["bypass"].value_or<bool>(false);
                bool autoload = tbl["autoload"].value_or<bool>(true);
                std::string windowTitle = tbl["window_title"].value_or<std::string>("");
                
                // If an existing plugin at index matches the dll path, reuse it!
                if (i < plugins.size() && plugins[i] && plugins[i]->GetPath() == path) {
                    LogDebug("Delta reload: Reusing plugin at index " + std::to_string(i) + ": " + path);
                    std::unique_ptr<VstPlugin> p = std::move(plugins[i]);
                    
                    p->SetAutoloadEnabled(autoload);
                    p->SetBypassed(bypass);
                    
                    // Reload preset if its path has changed
                    if (p->GetPresetPath() != preset) {
                        p->SetPresetPath(preset);
                        p->LoadPreset(preset);
                    }
                    
                    p->CheckUiState(showUi);
                    newPlugins.push_back(std::move(p));
                } else {
                    LogDebug("Delta reload: Instantiating new plugin at index " + std::to_string(i) + ": " + path);
                    auto p = std::make_unique<VstPlugin>(path, preset, showUi, (int)(i + 1), windowTitle, this);
                    p->SetAutoloadEnabled(autoload);
                    p->SetBypassed(bypass);
                    if (p->Load()) {
                        p->SetSampleRateAndBlockSize(currentSampleRate, currentBlockSize);
                        newPlugins.push_back(std::move(p));
                        LogDebug("New plugin added to chain");
                    } else {
                        LogDebug("Plugin load failed: " + path);
                    }
                }
            }
            
            // Remaining old plugins are automatically cleaned up when old plugins vector is replaced
            plugins = std::move(newPlugins);
            LogDebug("Config delta reload finished successfully");
        } else {
            LogDebug("No chain array found in TOML, clearing plugins");
            plugins.clear();
        }
    } catch (const std::exception& e) {
        LogDebug("Exception caught during TOML parse/load: " + std::string(e.what()));
    }
    LogDebug("VstChain::ReloadConfig() finish");
}

void VstChain::WatchThread() {
    LogDebug("VstChain::WatchThread() start");
    hostThreadId = GetCurrentThreadId();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool shouldUninit = SUCCEEDED(hr) || hr == S_FALSE;
    LogDebug("CoInitializeEx (APARTMENTTHREADED) called, result: " + std::to_string(hr));

    // Force creation of the message queue for this thread
    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WIN32_FILE_ATTRIBUTE_DATA lastData;
    ZeroMemory(&lastData, sizeof(lastData));
    std::wstring wConfigPath = utf8ToUtf16(configPath);
    GetFileAttributesExW(wConfigPath.c_str(), GetFileExInfoStandard, &lastData);

    UINT_PTR timerId = SetTimer(nullptr, 0, 500, nullptr);

    while (hostThreadRunning) {
        BOOL bRet = GetMessage(&msg, nullptr, 0, 0);
        if (bRet == -1) {
            break;
        }
        else if (bRet == 0) {
            break;
        }

        if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            WIN32_FILE_ATTRIBUTE_DATA newData;
            if (GetFileAttributesExW(wConfigPath.c_str(), GetFileExInfoStandard, &newData)) {
                if (CompareFileTime(&lastData.ftLastWriteTime, &newData.ftLastWriteTime) != 0) {
                    LogDebug("Config file change detected!");
                    lastData = newData;
                    ReloadConfig();
                }
            }
        }
        else if (msg.message == (WM_USER + 101)) {
            LogDebug("Initial config load triggered!");
            ReloadConfig();
        }
        else {
            if (msg.message == WM_KEYDOWN) {
                bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrlPressed && (msg.wParam == 'S' || msg.wParam == 'L')) {
                    HWND targetHwnd = msg.hwnd;
                    VstPlugin* plugin = nullptr;
                    while (targetHwnd != nullptr) {
                        wchar_t clsName[256];
                        GetClassNameW(targetHwnd, clsName, 256);
                        if (wcsncmp(clsName, L"VstHostWindow_", 14) == 0) {
                            plugin = (VstPlugin*)GetWindowLongPtrW(targetHwnd, GWLP_USERDATA);
                            break;
                        }
                        targetHwnd = GetParent(targetHwnd);
                    }
                    if (plugin) {
                        if (msg.wParam == 'S') {
                            LogDebug("Ctrl+S detected on plugin window, saving preset...");
                            SavePluginState(plugin);
                        } else if (msg.wParam == 'L') {
                            LogDebug("Ctrl+L detected on plugin window, loading preset...");
                            std::string pPath = plugin->GetPresetPath();
                            if (!pPath.empty()) {
                                plugin->LoadPreset(pPath);
                            }
                        }
                    }
                }
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    KillTimer(nullptr, timerId);

    // Clear plugins under lock before CoUninitialize
    {
        std::lock_guard<std::mutex> lock(chainMutex);
        LogDebug("Clearing plugins inside WatchThread exit...");
        plugins.clear();
    }

    if (shouldUninit) {
        CoUninitialize();
        LogDebug("CoUninitialize called");
    }
    LogDebug("VstChain::WatchThread() exit");
}

void VstChain::SetSampleRateAndBlockSize(double sampleRate, int blockSize) {
    std::lock_guard<std::mutex> lock(chainMutex);
    currentSampleRate = sampleRate;
    int neededBlock = blockSize * 2;
    if (neededBlock > currentBlockSize) currentBlockSize = neededBlock;
    if (currentBlockSize < 16384) currentBlockSize = 16384;
    for (auto& p : plugins) {
        p->SetSampleRateAndBlockSize(sampleRate, blockSize);
    }
}

void VstChain::Process(float* interleavedBuffer, int numSamples, int numChannels) {
    std::lock_guard<std::mutex> lock(chainMutex);
    if (plugins.empty() || numChannels == 0 || numSamples == 0) return;
    
    if (planarChannels.size() != numChannels) {
        planarChannels.resize(numChannels);
        planarChannelPointers.resize(numChannels);
    }
    for (int c = 0; c < numChannels; ++c) {
        size_t targetCapacity = (size_t)numSamples > 16384 ? (size_t)numSamples : 16384;
        if (planarChannels[c].capacity() < targetCapacity) {
            planarChannels[c].reserve(targetCapacity);
        }
        if (planarChannels[c].size() < (size_t)numSamples) {
            planarChannels[c].resize(numSamples);
        }
        planarChannelPointers[c] = planarChannels[c].data();
    }
    
    // De-interleave
    for (int s = 0; s < numSamples; ++s) {
        for (int c = 0; c < numChannels; ++c) {
            planarChannels[c][s] = interleavedBuffer[s * numChannels + c];
        }
    }
    
    // Process chain
    for (auto& p : plugins) {
        if (p->IsBypassed()) {
            continue;
        }
        p->Process(planarChannelPointers.data(), planarChannelPointers.data(), numSamples, numChannels);
    }
    
    // Re-interleave
    for (int s = 0; s < numSamples; ++s) {
        for (int c = 0; c < numChannels; ++c) {
            interleavedBuffer[s * numChannels + c] = planarChannels[c][s];
        }
    }
}
