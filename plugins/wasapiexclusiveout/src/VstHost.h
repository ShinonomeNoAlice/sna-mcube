#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <vector>
#include <map>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <Windows.h>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <condition_variable>

using namespace Steinberg;
using namespace Steinberg::Vst;

class VstChain;

class VstPlugin {
public:
    VstPlugin(const std::string& path, const std::string& presetPath, bool showUi, int orderIndex, const std::string& customTitle, VstChain* chain);
    ~VstPlugin();

    bool Load();
    void Unload();

    void Process(float** inputs, float** outputs, int numSamples, int numChannels);
    void SetSampleRateAndBlockSize(double sampleRate, int blockSize);
    void Reset();
    
    void CheckUiState(bool showUi);
    void OnParameterEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    bool IsBypassed() const { return isBypassed; }
    void SetBypassed(bool b) { isBypassed = b; }
    bool IsAutoloadEnabled() const { return autoloadEnabled; }
    void SetAutoloadEnabled(bool b) { autoloadEnabled = b; }
    std::string GetPath() const { return dllPath; }
    std::string GetPresetPath() const { return presetPath; }
    void SetPresetPath(const std::string& path) { presetPath = path; }
    void SetLoadedPresetPath(const std::string& path) { loadedPresetPath = path; }
    int GetOrderIndex() const { return orderIndex; }
    void SavePreset(const std::string& path);
    void LoadPreset(const std::string& path);

private:
    void SetupProcessing();

    std::string dllPath;
    std::string presetPath;
    bool showUiDesired;

    VST3::Hosting::Module::Ptr module;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
    Steinberg::IPtr<Steinberg::IPlugView> view;
    
    HWND hwnd = nullptr;
    bool viewAttached = false;
    bool inResize = false;
    
    double currentSampleRate = 0.0;
    int currentBlockSize = 0;
    int64_t totalSamplesProcessed = 0; // Monotonic sample counter for VST ProcessContext
    int64_t streamStartSystemTime = 0; // High-precision anchor timestamp (ns) for deterministic systemTime
    
    std::mutex paramMutex;
    struct ParamChange {
        Steinberg::Vst::ParamID id;
        Steinberg::Vst::ParamValue value;
    };
    std::vector<ParamChange> pendingParamChanges;
    
    std::mutex outputParamMutex;
    std::map<Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue> latestOutputParams;
    
    int orderIndex = 0;
    std::string customTitle;
    std::string vstName;
    VstChain* chain = nullptr;
    std::string loadedPresetPath;
    Steinberg::FUID effectId;
    bool isBypassed = false;
    bool autoloadEnabled = true;
    
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};

class VstChain {
public:
    VstChain(const std::string& tomlConfigPath);
    ~VstChain();
    
    void SavePluginState(VstPlugin* plugin);
    std::string GetConfigPath() const { return configPath; }

    void Process(float* interleavedBuffer, int numSamples, int numChannels, int targetBlockSize = 0);
    void SetSampleRateAndBlockSize(double sampleRate, int blockSize);
    void Reset();
    double GetCurrentSampleRate() const { return currentSampleRate; }
    int GetCurrentBlockSize() const { return currentBlockSize; }

private:
    void ReloadConfig();
    void WatchThread();

    std::string configPath;
    std::vector<std::unique_ptr<VstPlugin>> plugins;
    
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    std::mutex chainMutex;
    std::thread hostThread;
    std::atomic<bool> hostThreadRunning{false};
    DWORD hostThreadId = 0;



    // Planar buffers for VST processing
    std::vector<std::vector<float>> planarChannels;
    std::vector<float*> planarChannelPointers;
};
