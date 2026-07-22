//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2004-2023 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#include "WasapiExclusiveOut.h"
#include <soxr.h>
#include <cmath>
#include <musikcore/sdk/constants.h>
#include <musikcore/sdk/IPreferences.h>
#include <musikcore/sdk/ISchema.h>
#include <musikcore/sdk/IDebug.h>
#include "VstHost.h"
#include <AudioSessionTypes.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <timeapi.h>
#include <mmsystem.h>
#include <fstream>
#include <string>

#define PREF_DEVICE_ID "device_id"
#define PREF_ENDPOINT_ROUTING "enable_audio_endpoint_routing"
#define PREF_BUFFER_LENGTH_SECONDS "buffer_length_seconds"
#define PREF_ALLOW_DECODER_RESAMPLING "allow_decoder_resampling"
#define PREF_DAC_SETTLING_MS "dac_settling_ms"
#define PREF_RELEASE_ON_PAUSE "release_device_on_pause"
#define PREF_ENABLE_TRACE_LOGGING "enable_trace_logging"
#define PREF_SOXR_OVERSAMPLING "soxr_oversampling"
#define PREF_SOXR_PRESET "soxr_preset"
#define PREF_SOXR_HEADROOM_DB "soxr_headroom_db"
#define PREF_SOXR_CUSTOM_PRECISION "soxr_custom_precision_bits"
#define PREF_SOXR_CUSTOM_PHASE "soxr_custom_phase_response_pct"
#define PREF_SOXR_CUSTOM_PASSBAND_END "soxr_custom_passband_end"
#define PREF_SOXR_CUSTOM_STOPBAND_BEGIN "soxr_custom_stopband_begin"
#define PREF_SOXR_CUSTOM_DOUBLE_PRECISION "soxr_custom_double_precision"
#define PREF_VST_ENABLED "vst_enabled"


using Lock = std::unique_lock<std::recursive_mutex>;
musik::core::sdk::IPreferences* prefs = nullptr;
musik::core::sdk::IDebug* debug = nullptr;

extern "C" __declspec(dllexport) void SetDebug(musik::core::sdk::IDebug* debug) {
    ::debug = debug;
}

static bool traceLoggingEnabled() {
    return ::prefs && prefs->GetBool(PREF_ENABLE_TRACE_LOGGING, false);
}

static std::string GetLogPath() {
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        std::string logPath = std::string(appdata) + "\\musikcube\\wasapiexclusive_debug.txt";
        free(appdata);
        return logPath;
    }
    return "";
}

static void LogInfo(const std::string& message) {
    if (::debug) {
        ::debug->Info("WasapiExclusiveOut", message.c_str());
    }
    if (traceLoggingEnabled()) {
        std::string path = GetLogPath();
        if (path.size() > 0) {
            std::ofstream log(path, std::ios::app);
            if (log) {
                log << "[INFO] " << message << std::endl;
            }
        }
    }
}

static void LogWarning(const std::string& message) {
    if (::debug) {
        ::debug->Warning("WasapiExclusiveOut", message.c_str());
    }
    if (traceLoggingEnabled()) {
        std::string path = GetLogPath();
        if (path.size() > 0) {
            std::ofstream log(path, std::ios::app);
            if (log) {
                log << "[WARNING] " << message << std::endl;
            }
        }
    }
}

static void LogError(const std::string& message) {
    if (::debug) {
        ::debug->Error("WasapiExclusiveOut", message.c_str());
    }
    std::string path = GetLogPath();
    if (path.size() > 0) {
        std::ofstream log(path, std::ios::app);
        if (log) {
            log << "[ERROR] " << message << std::endl;
        }
    }
}

static void LogDebug(const std::string& message) {
    if (traceLoggingEnabled()) {
        std::string path = GetLogPath();
        if (path.size() > 0) {
            std::ofstream log(path, std::ios::app);
            if (log) {
                log << "[DEBUG] " << message << std::endl;
            }
        }
    }
}

static inline std::string HresultToString(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08X", hr);
    return std::string(buf);
}

static inline std::string GuidToString(GUID guid) {
    char buf[64];
    sprintf_s(buf, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::string(buf);
}

static inline std::string utf16to8(const wchar_t* utf16) {
    if (!utf16) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, 0, 0, 0, 0);
    if (size <= 0) return "";
    
    std::string utf8str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16, -1, &utf8str[0], size, 0, 0);
    utf8str.resize(size - 1); // remove the null terminator that std::string handles
    
    return utf8str;
}

static std::string getDeviceId() {
    return getPreferenceString<std::string>(prefs, PREF_DEVICE_ID, "");
}

static std::string getVstConfigPath() {
    std::string path = "";
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        path = std::string(appdata) + "\\musikcube\\wasapiexclusive_vst.toml";
        free(appdata);
    }
    return path;
}

class WasapiExclusiveDevice : public musik::core::sdk::IDevice {
    public:
        WasapiExclusiveDevice(const std::string& id, const std::string& name) {
            this->id = id;
            this->name = name;
        }

        virtual void Release() override { delete this; }
        virtual const char* Name() const override { return name.c_str(); }
        virtual const char* Id() const override { return id.c_str(); }

    private:
        std::string name, id;
};

class WasapiExclusiveDeviceList : public musik::core::sdk::IDeviceList {
    public:
        virtual void Release() override { delete this; }
        virtual size_t Count() const override { return devices.size(); }
        virtual const IDevice* At(size_t index) const override { return &devices.at(index); }

        void Add(const std::string& id, const std::string& name) {
            devices.push_back(WasapiExclusiveDevice(id, name));
        }

    private:
        std::vector<WasapiExclusiveDevice> devices;
};

extern "C" __declspec(dllexport) void SetPreferences(musik::core::sdk::IPreferences* prefs) {
    ::prefs = prefs;
}

extern "C" __declspec(dllexport) musik::core::sdk::ISchema* GetSchema() {
    auto schema = new TSchema<>();
    // 1. Trace logging on top
    schema->AddBool(PREF_ENABLE_TRACE_LOGGING, false);

    // 2. Separator
    schema->AddEnum("---", { "---" }, "---");

    // 3. Standard WASAPI settings
    schema->AddBool(PREF_ENDPOINT_ROUTING, false);
    schema->AddDouble(PREF_BUFFER_LENGTH_SECONDS, 1.0, 2, 0.05, 5.0);
    schema->AddBool(PREF_ALLOW_DECODER_RESAMPLING, false);
    schema->AddInt(PREF_DAC_SETTLING_MS, 0, 0, 5000);
    schema->AddBool(PREF_RELEASE_ON_PAUSE, false);

    // 4. Separator
    schema->AddEnum("---", { "---" }, "---");

    // 5. Soxr general settings
    schema->AddEnum(PREF_SOXR_OVERSAMPLING, { "No Scaling", "2x", "4x", "8x", "16x", "Max (Integer Scaling)", "Max (Highest Supported)" }, "No Scaling");
    schema->AddEnum(PREF_SOXR_PRESET, { "Quick", "Low", "Medium", "High (Default)", "Very High", "Custom" }, "High (Default)");
    schema->AddDouble(PREF_SOXR_HEADROOM_DB, 0.0, 1, -12.0, 0.0);

    // 6. Custom Separator
    schema->AddEnum("--- Custom soxr settings (\"Custom\" preset) ---", { "---" }, "---");

    // 7. Soxr custom settings
    schema->AddInt(PREF_SOXR_CUSTOM_PRECISION, 20, 12, 32);
    schema->AddDouble(PREF_SOXR_CUSTOM_PHASE, 50.0, 1, 0.0, 100.0);
    schema->AddDouble(PREF_SOXR_CUSTOM_PASSBAND_END, 0.913, 3, 0.5, 0.99);
    schema->AddDouble(PREF_SOXR_CUSTOM_STOPBAND_BEGIN, 1.0, 3, 1.0, 1.2);
    schema->AddBool(PREF_SOXR_CUSTOM_DOUBLE_PRECISION, false);

    // 8. VST Host Settings
    schema->AddEnum("--- VST3 Host ---", { "---" }, "---");
    schema->AddBool(PREF_VST_ENABLED, true);

    return schema;
}

static bool audioRoutingEnabled() {
    return ::prefs && prefs->GetBool(PREF_ENDPOINT_ROUTING, false);
}

static bool allowDecoderResampling() {
    return ::prefs && prefs->GetBool(PREF_ALLOW_DECODER_RESAMPLING, false);
}

class NotificationClient : public IMMNotificationClient {
    public:
        NotificationClient(WasapiExclusiveOut* owner)
        : count(1)
        , owner(owner)
        , enumerator(nullptr) {
        }

        ~NotificationClient() {
            if (this->enumerator) {
                this->enumerator->Release();
                this->enumerator = nullptr;
            }
        }

        /* IUnknown methods -- AddRef, Release, and QueryInterface */

        ULONG STDMETHODCALLTYPE AddRef() {
            return InterlockedIncrement(&this->count);
        }

        ULONG STDMETHODCALLTYPE Release() {
            ULONG newCount = InterlockedDecrement(&this->count);
            if (0 == newCount) {
                delete this;
            }
            return newCount;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
            if (IID_IUnknown == riid) {
                this->AddRef();
                *ppvInterface = (IUnknown*)this;
            }
            else if (__uuidof(IMMNotificationClient) == riid) {
                this->AddRef();
                *ppvInterface = (IMMNotificationClient*)this;
            }
            else {
                *ppvInterface = nullptr;
                return E_NOINTERFACE;
            }
            return S_OK;
        }

        /* Callback methods for device-event notifications. */

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
            EDataFlow flow, ERole role,
            LPCWSTR pwstrDeviceId)
        {
            if (audioRoutingEnabled()) {
                if (flow == eRender && role == eMultimedia) {
                    if (this->lastDeviceId != std::wstring(pwstrDeviceId)) {
                        owner->OnDeviceChanged();
                        this->lastDeviceId = std::wstring(pwstrDeviceId);
                    }
                }
            }

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key){
            return S_OK;
        }

    private:
        LONG count;
        IMMDeviceEnumerator *enumerator;
        WasapiExclusiveOut* owner;
        std::wstring lastDeviceId;
};

WasapiExclusiveOut::WasapiExclusiveOut()
: enumerator(nullptr)
, device(nullptr)
, audioClient(nullptr)
, renderClient(nullptr)
, audioClock(nullptr)
, notificationClient(nullptr)
, outputBufferFrames(0)
, state(StateStopped)
, latency(0)
, configuredSampleRate(0)
, resampler(nullptr)
, deviceChanged(false)
, volume(1.0f)
, headroomMultiplier(1.0f) {
    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
    timeBeginPeriod(1);
    
}

WasapiExclusiveOut::~WasapiExclusiveOut() {
    timeEndPeriod(1);
}

void WasapiExclusiveOut::Release() {
    this->Reset();

    if (this->enumerator) {
        if (this->notificationClient) {
            this->enumerator->UnregisterEndpointNotificationCallback(this->notificationClient);
            this->notificationClient->Release();
            this->notificationClient = nullptr;
        }

        this->enumerator->Release();
        this->enumerator = nullptr;
    }

    delete this;
}

void WasapiExclusiveOut::Pause() {
    this->state = StatePaused;

    Lock lock(this->stateMutex);

    if (this->audioClient) {
        this->audioClient->Stop();
        if (::prefs && prefs->GetBool(PREF_RELEASE_ON_PAUSE, false)) {
            LogInfo("release_device_on_pause is enabled. Releasing exclusive lock on Pause.");
            this->Reset();
        }
    }
}

void WasapiExclusiveOut::Resume() {
    this->state = StatePlaying;

    Lock lock(this->stateMutex);

    if (this->audioClient) {
        this->audioClient->Start();
    }
}

void WasapiExclusiveOut::SetVolume(double volume) {
    Lock lock(this->stateMutex);
    this->volume = volume;
}

double WasapiExclusiveOut::GetVolume() {
    return this->volume;
}

void WasapiExclusiveOut::Stop() {
    this->state = StateStopped;

    Lock lock(this->stateMutex);
    this->Reset();
}

void WasapiExclusiveOut::Drain() {
    int sleepMs = (int) (round(this->Latency()) * 1000.0f);

    while (this->state != StateStopped && sleepMs > 0) {
        Sleep(50);
        if (this->state == StatePlaying) {
            sleepMs -= 50;
        }
    }
}

OutputState WasapiExclusiveOut::Play(IBuffer *buffer, IBufferProvider *provider) {
    Lock lock(this->stateMutex);

    if (this->state == StatePaused) {
        return OutputState::InvalidState;
    }

    if (this->deviceChanged) {
        this->Reset();
        this->deviceChanged = false;
        return OutputState::FormatError;
    }

    if (!this->Configure(buffer)) {
        this->Reset();
        return OutputState::FormatError;
    }

    HRESULT result;
    UINT32 availableFrames = 0;
    UINT32 frameOffset = 0;
    
    float* src = buffer->BufferPointer();
    UINT32 samples = (UINT32)buffer->Samples();
    UINT32 framesToWrite = samples / (UINT32)buffer->Channels();

    // Calculate how many frames we expect to output from resampling
    UINT32 expectedOutputFrames = framesToWrite;
    if (this->resampler) {
        expectedOutputFrames = (UINT32)((double)framesToWrite * (double)this->configuredSampleRate / (double)buffer->SampleRate()) + 16;
    }

    // Rationale for checking WASAPI buffer capacity before resampling:
    // To prevent buffer underruns and stutter, we must check if WASAPI has enough available frames
    // for the expected resampled output *before* calling the expensive soxr_process().
    // If WASAPI is full/lacks space, we return the calculated sleep delta immediately. This allows
    // the host thread to yield and sleep without wasting CPU cycles on resampling that cannot be written,
    // and prevents sample accumulation/timing misalignment.
    if (this->audioClient->GetCurrentPadding(&frameOffset) == S_OK) {
        availableFrames = (this->outputBufferFrames - frameOffset);
        if (availableFrames < expectedOutputFrames) {
            UINT32 delta = expectedOutputFrames - availableFrames;
            return (OutputState) ((delta * 1000) / this->configuredSampleRate);
        }
    } else {
        return OutputState::FormatError;
    }

    // Now that we know we have enough space, run the resampler
    if (this->resampler) {
        UINT32 inputFrames = framesToWrite;
        UINT32 maxOutputFrames = expectedOutputFrames;
        UINT32 channels = (UINT32)buffer->Channels();
        UINT32 neededSize = maxOutputFrames * channels;
        
        // Rationale for buffer underrun/stutter fix:
        // We ensure the buffer is pre-allocated in Configure() so that we avoid allocating/zero-initializing
        // memory in the real-time audio thread's Play() function. If we perform allocations in Play(), 
        // OS scheduling delays can starve the WASAPI buffer, leading to audible stutters/pops (buffer underrun).
        // We only resize as a safety fallback if the incoming audio chunk is unexpectedly larger.
        if (this->resampleBuffer.size() < neededSize) {
            LogWarning("Resample buffer size was smaller than needed. Resizing in real-time hot path! (Current: " +
                       std::to_string(this->resampleBuffer.size()) + ", Needed: " + std::to_string(neededSize) + ")");
            this->resampleBuffer.resize(neededSize);
        }
        
        size_t idone = 0;
        size_t odone = 0;
        soxr_error_t err = soxr_process(
            (soxr_t)this->resampler,
            buffer->BufferPointer(),
            inputFrames,
            &idone,
            this->resampleBuffer.data(),
            maxOutputFrames,
            &odone
        );
        if (err) {
            LogError("soxr_process failed: " + std::string(soxr_strerror(err)));
            return OutputState::FormatError;
        }
        
        src = this->resampleBuffer.data();
        framesToWrite = (UINT32)odone;
        samples = framesToWrite * channels;
    }
    
    // Process VST chain after upsampling/format conversion
    bool vstEnabled = ::prefs && prefs->GetBool(PREF_VST_ENABLED, true);
    if (vstEnabled) {
        if (!this->vstChain) {
            std::string tomlPath = getVstConfigPath();
            this->vstChain = std::make_unique<VstChain>(tomlPath);
        }
        this->vstChain->SetSampleRateAndBlockSize(this->configuredSampleRate, framesToWrite);
        this->vstChain->Process(src, framesToWrite, buffer->Channels());
    } else {
        if (this->vstChain) {
            this->vstChain.reset();
        }
    }

    if (availableFrames >= framesToWrite) {
        BYTE *data = nullptr;
        result = this->renderClient->GetBuffer(framesToWrite, &data);
        if (result == S_OK) {
            float vol = (float)this->volume;
            if (this->resampler) {
                vol *= this->headroomMultiplier;
            }

            if (this->targetFormatType == FormatFloat32) {
                float* dst = (float*) data;
                for (UINT32 i = 0; i < samples; ++i) {
                    dst[i] = src[i] * vol;
                }
            }
            else if (this->targetFormatType == FormatPCM32) {
                int32_t* dst = (int32_t*) data;
                for (UINT32 i = 0; i < samples; ++i) {
                    float s = src[i] * vol;
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    dst[i] = (int32_t)(s * 2147483647.0);
                }
            }
            else if (this->targetFormatType == FormatPCM24In32) {
                int32_t* dst = (int32_t*) data;
                for (UINT32 i = 0; i < samples; ++i) {
                    float s = src[i] * vol;
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    dst[i] = ((int32_t)(s * 8388607.0f)) << 8;
                }
            }
            else if (this->targetFormatType == FormatPCM24Packed) {
                uint8_t* dst = (uint8_t*) data;
                for (UINT32 i = 0; i < samples; ++i) {
                    float s = src[i] * vol;
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    int32_t val = (int32_t)(s * 8388607.0f);
                    dst[3 * i] = val & 0xFF;
                    dst[3 * i + 1] = (val >> 8) & 0xFF;
                    dst[3 * i + 2] = (val >> 16) & 0xFF;
                }
            }
            else if (this->targetFormatType == FormatPCM16) {
                int16_t* dst = (int16_t*) data;
                for (UINT32 i = 0; i < samples; ++i) {
                    float s = src[i] * vol;
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    dst[i] = (int16_t)(s * 32767.0f);
                }
            }

            this->renderClient->ReleaseBuffer(framesToWrite, 0);
            provider->OnBufferProcessed(buffer);
            return OutputState::BufferWritten;
        }
    }

    return OutputState::BufferFull;
}

void WasapiExclusiveOut::Reset() {
    Lock lock(this->stateMutex);

    if (this->resampler) {
        soxr_delete((soxr_t)this->resampler);
        this->resampler = nullptr;
    }

    if (this->audioClock) {
        this->audioClock->Release();
    }

    if (this->renderClient) {
        this->renderClient->Release();
    }

    if (this->audioClient) {
        this->audioClient->Reset();
        this->audioClient->Stop();
        this->audioClient->Release();
    }

    if (this->device) {
        this->device->Release();
    }

    this->audioClock = nullptr;
    this->renderClient = nullptr;
    this->audioClient = nullptr;
    this->device = nullptr;

    ZeroMemory(&waveFormat, sizeof(WAVEFORMATEXTENSIBLE));
}

double WasapiExclusiveOut::Latency() {
    return this->latency;
}

bool WasapiExclusiveOut::SetDefaultDevice(const char* deviceId) {
    return setDefaultDevice<IPreferences, WasapiExclusiveDevice, IOutput>(prefs, this, PREF_DEVICE_ID, deviceId);
}

IDevice* WasapiExclusiveOut::GetDefaultDevice() {
    return findDeviceById<WasapiExclusiveDevice, IOutput>(this, getDeviceId());
}

IDeviceList* WasapiExclusiveOut::GetDeviceList() {
    WasapiExclusiveDeviceList* result = new WasapiExclusiveDeviceList();
    IMMDeviceEnumerator *deviceEnumerator = nullptr;
    IMMDeviceCollection *deviceCollection = nullptr;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**) &deviceEnumerator);

    if (hr == S_OK) {
        hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
        if (hr == S_OK) {
            UINT deviceCount = 0;
            if (deviceCollection->GetCount(&deviceCount) == S_OK) {
                for (UINT i = 0; i < deviceCount; i++) {
                    IMMDevice* device = nullptr;
                    LPWSTR deviceIdPtr;
                    std::string deviceId, deviceName;

                    hr = deviceCollection->Item(i, &device);
                    if (hr == S_OK) {
                        if (device->GetId(&deviceIdPtr) == S_OK) {
                            deviceId = utf16to8(deviceIdPtr);
                            CoTaskMemFree(deviceIdPtr);
                        }

                        IPropertyStore *propertyStore;
                        if (device->OpenPropertyStore(STGM_READ, &propertyStore) == S_OK) {
                            PROPVARIANT friendlyName;
                            PropVariantInit(&friendlyName);

                            if (propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName) == S_OK) {
                                deviceName = utf16to8(friendlyName.pwszVal);
                                PropVariantClear(&friendlyName);
                            }

                            propertyStore->Release();
                        }

                        if (deviceId.size() || deviceName.size()) {
                            result->Add(deviceId, deviceName);
                        }

                        device->Release();
                    }
                }
            }

            deviceCollection->Release();
        }

        deviceEnumerator->Release();
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return result;
}

IMMDevice* WasapiExclusiveOut::GetPreferredDevice() {
    IMMDevice* result = nullptr;

    std::string storedDeviceId = getDeviceId();
    if (storedDeviceId.size() > 0) {
        IMMDeviceCollection *deviceCollection = nullptr;

        if (this->enumerator) {
            HRESULT hr = this->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
            if (hr == S_OK) {
                UINT deviceCount = 0;
                if (deviceCollection->GetCount(&deviceCount) == S_OK) {
                    for (UINT i = 0; i < deviceCount; i++) {
                        IMMDevice* device = nullptr;
                        LPWSTR deviceIdPtr;

                        hr = deviceCollection->Item(i, &device);
                        if (hr == S_OK) {
                            if (device->GetId(&deviceIdPtr) == S_OK) {
                                if (storedDeviceId == utf16to8(deviceIdPtr)) {
                                    result = device;
                                }

                                CoTaskMemFree(deviceIdPtr);

                                if (result == device) { /* found it! */
                                    goto found_or_done;
                                }
                            }

                            device->Release();
                        }
                    }
                }
found_or_done:
                deviceCollection->Release();
            }
        }
    }

    return result;
}

int WasapiExclusiveOut::GetDefaultSampleRate() {
    int result = -1;
    Lock lock(this->stateMutex);
    if (allowDecoderResampling()) {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        this->InitializeAudioClient();
        if (this->audioClient) {
            WAVEFORMATEX* deviceFormat = nullptr;
            audioClient->GetMixFormat(&deviceFormat);
            if (deviceFormat) {
                result = deviceFormat->nSamplesPerSec;
                CoTaskMemFree(deviceFormat);
            }
        }
        this->Reset();
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
    }
    return result;
}

bool WasapiExclusiveOut::InitializeAudioClient() {
    /* assumes stateMutex is locked */
    LogDebug("InitializeAudioClient() start");

    // Initialize COM on the current thread for IAudioClient activation.
    // NOTE: We do not call CoUninitialize() in Reset() because Reset() is frequently called
    // from different thread contexts (e.g. Stop() from the main UI thread, or state transitions)
    // than the playback thread where InitializeAudioClient() was invoked. Uninitializing COM
    // on a thread that we did not explicitly initialize, or doing so prematurely for a host thread,
    // can break the host's apartment state. Instead, we let the host thread manage its own
    // COM thread lifecycle when the thread exits.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT result = S_FALSE;

    if (!this->audioClient) {
        if (!this->enumerator) {
            result = CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                NULL,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                (void**)&this->enumerator);

            if (result != S_OK) {
                LogDebug("CoCreateInstance MMDeviceEnumerator failed, HRESULT=" + std::to_string(result));
                return false;
            }

            if (audioRoutingEnabled()) {
                this->notificationClient = new NotificationClient(this);

                if ((result = this->enumerator->RegisterEndpointNotificationCallback(this->notificationClient)) != S_OK) {
                    LogDebug("RegisterEndpointNotificationCallback failed, HRESULT=" + std::to_string(result));
                    return false;
                }
            }
        }
    }

    if (!this->device) {
        bool preferredDeviceOk = false;

        IMMDevice* preferredDevice = this->GetPreferredDevice();
        if (preferredDevice) {
            LPWSTR idPtr = nullptr;
            preferredDevice->GetId(&idPtr);
            if (idPtr) {
                LogDebug("Preferred device selected by ID: " + utf16to8(idPtr));
                CoTaskMemFree(idPtr);
            }
            if ((result = preferredDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient)) == S_OK) {
                preferredDeviceOk = true;
                this->device = preferredDevice;
                LogDebug("Activated IAudioClient on preferred device successfully");
            } else {
                LogDebug("IAudioClient activation on preferred device failed, HRESULT=" + std::to_string(result));
            }
        }

        if (!preferredDeviceOk) {
            if (preferredDevice) {
                preferredDevice->Release();
            }

            if ((result = this->enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &this->device)) != S_OK) {
                LogDebug("GetDefaultAudioEndpoint failed, HRESULT=" + std::to_string(result));
                return false;
            }
            LogDebug("Fallback to default audio endpoint selected");

            if ((result = this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient)) != S_OK) {
                LogDebug("IAudioClient activation on default device failed, HRESULT=" + std::to_string(result));
                return false;
            }
            LogDebug("Activated IAudioClient on default device successfully");
        }
    }

    return true;
}

bool WasapiExclusiveOut::Configure(IBuffer *buffer) {
    /* assumes stateMutex is locked */
    LogDebug("Configure called: nChannels=" + std::to_string(buffer->Channels()) + 
             ", nSamplesPerSec=" + std::to_string(buffer->SampleRate()));

    if (this->audioClient &&
        waveFormat.Format.nChannels == buffer->Channels() &&
        this->rate == buffer->SampleRate())
    {
        LogDebug("Configure early return (already configured)");
        return true;
    }

    // Cache the previous configuration to check if we can perform a quick initialize on seek/re-route
    WAVEFORMATEXTENSIBLE cachedWf = this->waveFormat;
    TargetFormatType cachedFormatType = this->targetFormatType;
    int cachedInputRate = this->rate;
    int cachedConfiguredRate = this->configuredSampleRate;
    bool canUseCache = (cachedInputRate == buffer->SampleRate() && 
                        cachedWf.Format.nChannels == buffer->Channels() && 
                        cachedWf.Format.nSamplesPerSec > 0);

    this->Reset();
    this->InitializeAudioClient();

    if (!this->audioClient) {
        LogError("Configure: audioClient is null, failing");
        return false;
    }

    int mixFormatSampleRate = 48000;
    WAVEFORMATEX* mixFormat = nullptr;
    if (this->audioClient->GetMixFormat(&mixFormat) == S_OK) {
        mixFormatSampleRate = mixFormat->nSamplesPerSec;
        LogInfo("GetMixFormat returned: wFormatTag=" + std::to_string(mixFormat->wFormatTag) +
                ", nChannels=" + std::to_string(mixFormat->nChannels) +
                ", nSamplesPerSec=" + std::to_string(mixFormat->nSamplesPerSec) +
                ", wBitsPerSample=" + std::to_string(mixFormat->wBitsPerSample) +
                ", cbSize=" + std::to_string(mixFormat->cbSize));
        
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* mixExt = (WAVEFORMATEXTENSIBLE*)mixFormat;
            LogInfo("GetMixFormat Extensible details: dwChannelMask=" + std::to_string(mixExt->dwChannelMask) +
                     ", wValidBitsPerSample=" + std::to_string(mixExt->Samples.wValidBitsPerSample) +
                     ", SubFormat=" + GuidToString(mixExt->SubFormat));
        }

        HRESULT testHr = this->audioClient->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            mixFormat,
            NULL
        );
        LogInfo("IsFormatSupported for MixFormat in Exclusive Mode returned HRESULT=" + HresultToString(testHr));

        CoTaskMemFree(mixFormat);
    }

    DWORD speakerConfig = 0;
    switch (buffer->Channels()) {
        case 1:
            speakerConfig = KSAUDIO_SPEAKER_MONO;
            break;
        case 2:
            speakerConfig = KSAUDIO_SPEAKER_STEREO;
            break;
        case 4:
            speakerConfig = KSAUDIO_SPEAKER_QUAD;
            break;
        case 5:
            speakerConfig = (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
            break;
        case 6:
            speakerConfig = KSAUDIO_SPEAKER_5POINT1;
            break;
    }

    // Rationale for querying device period (timing and periodicity):
    // WASAPI exclusive mode expects the buffer duration and update intervals to align closely
    // with the hardware's internal packet size. We query defaultPeriod and minimumPeriod as
    // baseline constraints so we can ensure the timing candidates we evaluate match the device's
    // hardware interrupts, preventing buffer timing misalignment, clicks, or dropouts.
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    HRESULT result = this->audioClient->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    if (result != S_OK) {
        LogError("GetDevicePeriod failed, HRESULT=" + HresultToString(result));
        return false;
    }
    LogInfo("GetDevicePeriod returned defaultPeriod=" + std::to_string(defaultPeriod) + 
             ", minimumPeriod=" + std::to_string(minimumPeriod));

    WAVEFORMATEXTENSIBLE &wf = this->waveFormat;

    struct FormatTarget {
        WORD bitsPerSample;
        WORD validBitsPerSample;
        GUID subFormat;
        TargetFormatType formatType;
        const char* name;
    };

    FormatTarget targets[] = {
        { 32, 32, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, FormatFloat32, "IEEE Float" },
        { 32, 32, KSDATAFORMAT_SUBTYPE_PCM, FormatPCM32, "PCM 32" },
        { 32, 24, KSDATAFORMAT_SUBTYPE_PCM, FormatPCM24In32, "PCM 24-in-32" },
        { 24, 24, KSDATAFORMAT_SUBTYPE_PCM, FormatPCM24Packed, "PCM 24 Packed" },
        { 16, 16, KSDATAFORMAT_SUBTYPE_PCM, FormatPCM16, "PCM 16" }
    };

    double bufferLengthSeconds = ::prefs->GetDouble(PREF_BUFFER_LENGTH_SECONDS, 1.0);
    REFERENCE_TIME preferredDuration = REFERENCE_TIME (1000.0 * 1000.0 * 10.0 * bufferLengthSeconds);

    std::vector<REFERENCE_TIME> periodicityCandidates;
    periodicityCandidates.push_back(defaultPeriod);
    if (minimumPeriod != defaultPeriod) {
        periodicityCandidates.push_back(minimumPeriod);
    }

    // Determine target sample rates to try
    std::string oversampling = getPreferenceString<std::string>(prefs, PREF_SOXR_OVERSAMPLING, "No Scaling");
    DWORD nativeRate = buffer->SampleRate();
    std::vector<DWORD> ratesToTry;

    if (oversampling == "Max (Highest Supported)") {
        std::vector<DWORD> highRates = {
            768000UL, 705600UL, 384000UL, 352800UL, 192000UL, 176400UL, 96000UL, 88200UL, 48000UL, 44100UL
        };
        for (DWORD r : highRates) {
            ratesToTry.push_back(r);
        }
        ratesToTry.push_back(nativeRate);
        ratesToTry.push_back((DWORD)mixFormatSampleRate);
    }
    else if (oversampling == "Max (Integer Scaling)") {
        std::vector<DWORD> multiples = { 16, 8, 4, 2, 1 };
        for (DWORD m : multiples) {
            ratesToTry.push_back(nativeRate * m);
        }
        ratesToTry.push_back((DWORD)mixFormatSampleRate);
        for (DWORD r : { 192000UL, 96000UL, 48000UL, 44100UL }) {
            ratesToTry.push_back(r);
        }
    }
    else {
        int mult = 1;
        if (oversampling == "2x") mult = 2;
        else if (oversampling == "4x") mult = 4;
        else if (oversampling == "8x") mult = 8;
        else if (oversampling == "16x") mult = 16;

        if (mult > 1) {
            ratesToTry.push_back(nativeRate * mult);
            for (int m = mult / 2; m >= 1; m /= 2) {
                ratesToTry.push_back(nativeRate * m);
            }
            ratesToTry.push_back((DWORD)mixFormatSampleRate);
            for (DWORD r : { 192000UL, 96000UL, 48000UL, 44100UL }) {
                ratesToTry.push_back(r);
            }
        }
        else {
            ratesToTry.push_back(nativeRate);
            ratesToTry.push_back((DWORD)mixFormatSampleRate);
            for (DWORD r : { 48000UL, 44100UL, 96000UL, 192000UL }) {
                if (std::find(ratesToTry.begin(), ratesToTry.end(), r) == ratesToTry.end()) {
                    ratesToTry.push_back(r);
                }
            }
        }
    }

    // De-duplicate ratesToTry while preserving order
    std::vector<DWORD> uniqueRates;
    for (DWORD r : ratesToTry) {
        if (std::find(uniqueRates.begin(), uniqueRates.end(), r) == uniqueRates.end()) {
            uniqueRates.push_back(r);
        }
    }
    ratesToTry = uniqueRates;

    bool rateChanged = true;
    bool initialized = false;

    if (canUseCache) {
        LogInfo("Attempting quick initialization with cached format to skip negotiation...");
        this->waveFormat = cachedWf;
        this->targetFormatType = cachedFormatType;

        for (REFERENCE_TIME periodicity : periodicityCandidates) {
            std::vector<REFERENCE_TIME> bufferCandidates;
            bufferCandidates.push_back(preferredDuration);
            for (int mult : {16, 8, 4, 2}) {
                REFERENCE_TIME d = defaultPeriod * mult;
                if (d < preferredDuration && d >= periodicity) {
                    bufferCandidates.push_back(d);
                }
            }
            if (periodicity < preferredDuration) {
                if (std::find(bufferCandidates.begin(), bufferCandidates.end(), periodicity) == bufferCandidates.end()) {
                    bufferCandidates.push_back(periodicity);
                }
            }

            for (REFERENCE_TIME bufferDuration : bufferCandidates) {
                if (!this->audioClient) {
                    if (this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient) != S_OK) {
                        continue;
                    }
                }

                result = this->audioClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    0,
                    bufferDuration,
                    periodicity,
                    (WAVEFORMATEX *) &this->waveFormat,
                    NULL
                );

                if (result == S_OK) {
                    initialized = true;
                    this->configuredSampleRate = cachedConfiguredRate;
                    rateChanged = false; // Same configured rate, no clock change
                    LogInfo("Quick initialization successful: rate=" + std::to_string(this->configuredSampleRate) + 
                            ", format type=" + std::to_string(this->targetFormatType));
                    break;
                }

                if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 alignedFrames = 0;
                    if (this->audioClient->GetBufferSize(&alignedFrames) == S_OK) {
                        REFERENCE_TIME alignedDuration = (REFERENCE_TIME)((10000000.0 * alignedFrames / cachedWf.Format.nSamplesPerSec) + 0.5);
                        
                        this->audioClient->Release();
                        this->audioClient = nullptr;
                        
                        if (this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient) == S_OK) {
                            result = this->audioClient->Initialize(
                                AUDCLNT_SHAREMODE_EXCLUSIVE,
                                0,
                                alignedDuration,
                                periodicity,
                                (WAVEFORMATEX *) &this->waveFormat,
                                NULL
                            );
                            if (result == S_OK) {
                                initialized = true;
                                this->configuredSampleRate = cachedConfiguredRate;
                                rateChanged = false; // Same configured rate, no clock change
                                LogInfo("Quick initialization successful (aligned): rate=" + std::to_string(this->configuredSampleRate));
                                break;
                            }
                        }
                    }
                }

                if (this->audioClient) {
                    this->audioClient->Release();
                    this->audioClient = nullptr;
                }
            }
            if (initialized) {
                break;
            }
        }

        if (!initialized) {
            LogWarning("Quick initialization failed. Falling back to full format negotiation.");
            this->Reset();
            this->InitializeAudioClient();
        }
    }

    for (DWORD targetRate : ratesToTry) {
        bool formatFoundForRate = false;
        const FormatTarget* selectedTarget = nullptr;
        
        for (const auto& target : targets) {
            ZeroMemory(&wf, sizeof(WAVEFORMATEXTENSIBLE));
            wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.Format.nChannels = (WORD) buffer->Channels();
            wf.Format.wBitsPerSample = target.bitsPerSample;
            wf.Format.nSamplesPerSec = targetRate;
            wf.Samples.wValidBitsPerSample = target.validBitsPerSample;
            wf.Format.nBlockAlign = (wf.Format.wBitsPerSample / 8) * wf.Format.nChannels;
            wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
            wf.dwChannelMask = speakerConfig;
            wf.SubFormat = target.subFormat;

            result = this->audioClient->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                (WAVEFORMATEX *) &wf,
                NULL
            );

            if (result == S_OK) {
                // Verify format support with a real test Initialize
                // Workaround: Virtual Audio Cable (VAC) by Eugene Muzychenko reports support
                // for 32-bit float in IsFormatSupported(), but actually attempting to Initialize()
                // the client with that format consistently fails/returns 0x88890008 (AUDCLNT_E_UNSUPPORTED_FORMAT).
                // Thus, we must perform a trial test Initialize to be absolutely sure the format is usable.
                IAudioClient* testClient = nullptr;
                if (this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&testClient) == S_OK) {
                    HRESULT initHr = testClient->Initialize(
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        0,
                        defaultPeriod,
                        defaultPeriod,
                        (WAVEFORMATEX*)&wf,
                        NULL
                    );
                    testClient->Release();
                    
                    if (initHr == S_OK) {
                        this->targetFormatType = target.formatType;
                        formatFoundForRate = true;
                        selectedTarget = &target;
                        LogInfo("Found and verified format: " + std::string(target.name) + 
                                 " at sample rate " + std::to_string(targetRate));
                        break;
                    }
                }
            }
        }

        if (formatFoundForRate && selectedTarget) {
            REFERENCE_TIME duration = preferredDuration;
            if (duration > 2000000) {
                duration = 2000000;
            }
            if (duration < defaultPeriod) {
                duration = defaultPeriod;
            }

            // Search for compatible buffer durations and periodicity intervals.
            // We use multiples of defaultPeriod to ensure synchronization with the driver's
            // timing loop, which prevents sample rate mismatch stuttering or click/pop issues.
            for (REFERENCE_TIME periodicity : periodicityCandidates) {
                std::vector<REFERENCE_TIME> bufferCandidates;
                bufferCandidates.push_back(duration);
                for (int mult : {16, 8, 4, 2}) {
                    REFERENCE_TIME d = defaultPeriod * mult;
                    if (d < duration && d >= periodicity) {
                        bufferCandidates.push_back(d);
                    }
                }
                if (periodicity < duration) {
                    if (std::find(bufferCandidates.begin(), bufferCandidates.end(), periodicity) == bufferCandidates.end()) {
                        bufferCandidates.push_back(periodicity);
                    }
                }

                for (REFERENCE_TIME bufferDuration : bufferCandidates) {
                    if (!this->audioClient) {
                        if (this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient) != S_OK) {
                            LogError("Failed to re-activate audioClient during candidate loop");
                            continue;
                        }
                    }

                    result = this->audioClient->Initialize(
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        0,
                        bufferDuration,
                        periodicity,
                        (WAVEFORMATEX *) &wf,
                        NULL
                    );
                    LogDebug("Initialize with bufferDuration " + std::to_string(bufferDuration) + 
                             " and periodicity " + std::to_string(periodicity) + 
                             " at sample rate " + std::to_string(targetRate) + 
                             " returned HRESULT=" + HresultToString(result));

                    if (result == S_OK) {
                        initialized = true;
                        this->configuredSampleRate = targetRate;
                        LogInfo("IAudioClient successfully initialized: rate=" + std::to_string(targetRate) + 
                                ", bufferDuration=" + std::to_string(bufferDuration) + 
                                ", periodicity=" + std::to_string(periodicity) + 
                                ", format=" + std::string(selectedTarget->name));
                        break;
                    }

                    if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                        UINT32 alignedFrames = 0;
                        if (this->audioClient->GetBufferSize(&alignedFrames) == S_OK) {
                            REFERENCE_TIME alignedDuration = (REFERENCE_TIME)((10000000.0 * alignedFrames / targetRate) + 0.5);
                            
                            this->audioClient->Release();
                            this->audioClient = nullptr;
                            
                            if (this->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audioClient) == S_OK) {
                                result = this->audioClient->Initialize(
                                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                                    0,
                                    alignedDuration,
                                    periodicity,
                                    (WAVEFORMATEX *) &wf,
                                    NULL
                                );
                                LogDebug("Re-Initialize with aligned duration " + std::to_string(alignedDuration) + 
                                         " and periodicity " + std::to_string(periodicity) +
                                         " returned HRESULT=" + HresultToString(result));
                                if (result == S_OK) {
                                    initialized = true;
                                    this->configuredSampleRate = targetRate;
                                    LogInfo("IAudioClient successfully initialized (aligned): rate=" + std::to_string(targetRate) + 
                                            ", bufferDuration=" + std::to_string(alignedDuration) + 
                                            ", periodicity=" + std::to_string(periodicity) + 
                                            ", format=" + std::string(selectedTarget->name));
                                    break;
                                }
                            }
                        }
                    }

                    if (this->audioClient) {
                        this->audioClient->Release();
                        this->audioClient = nullptr;
                    }
                }

                if (initialized) {
                    break;
                }
            }
        }

        if (initialized) {
            rateChanged = (cachedConfiguredRate != this->configuredSampleRate);
            break;
        }
    }

    if (!initialized) {
        LogError("Failed to initialize IAudioClient with any target sample rate and format");
        return false;
    }

    if ((result = this->audioClient->GetBufferSize(&this->outputBufferFrames)) != S_OK) {
        return false;
    }

    this->latency = (float) outputBufferFrames / (float) this->configuredSampleRate;

    if ((result = this->audioClient->GetService(__uuidof(IAudioRenderClient), (void**) &this->renderClient)) != S_OK) {
        return false;
    }

    if ((result = this->audioClient->GetService(__uuidof(IAudioClock), (void**) &this->audioClock)) != S_OK) {
        return false;
    }

    if ((result = this->audioClient->Start()) != S_OK) {
        return false;
    }

    this->state = StatePlaying;
    this->SetVolume(this->volume);

    // Create or reset soxr resampler
    if (this->resampler) {
        soxr_delete((soxr_t)this->resampler);
        this->resampler = nullptr;
    }

    if (this->configuredSampleRate != buffer->SampleRate()) {
        soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        
        // Build quality spec based on preset vs custom settings
        std::string preset = getPreferenceString<std::string>(prefs, PREF_SOXR_PRESET, "High (Default)");
        unsigned long recipe = SOXR_HQ;
        unsigned long flags = SOXR_HI_PREC_CLOCK;
        std::string debugQualityName = preset;

        if (preset == "Quick") {
            recipe = SOXR_QQ;
        }
        else if (preset == "Low") {
            recipe = SOXR_LQ;
        }
        else if (preset == "Medium") {
            recipe = SOXR_MQ;
        }
        else if (preset == "High (Default)") {
            recipe = SOXR_HQ;
        }
        else if (preset == "Very High") {
            recipe = SOXR_VHQ;
        }

        bool doublePrec = prefs ? prefs->GetBool(PREF_SOXR_CUSTOM_DOUBLE_PRECISION, false) : false;
        if (preset == "Custom") {
            if (doublePrec) {
                flags |= SOXR_DOUBLE_PRECISION;
            }
        }

        soxr_quality_spec_t q_spec = soxr_quality_spec(recipe, flags);

        if (preset == "Custom") {
            q_spec.precision = prefs ? (double)prefs->GetInt(PREF_SOXR_CUSTOM_PRECISION, 20) : 20.0;
            q_spec.phase_response = prefs ? prefs->GetDouble(PREF_SOXR_CUSTOM_PHASE, 50.0) : 50.0;
            q_spec.passband_end = prefs ? prefs->GetDouble(PREF_SOXR_CUSTOM_PASSBAND_END, 0.913) : 0.913;
            q_spec.stopband_begin = prefs ? prefs->GetDouble(PREF_SOXR_CUSTOM_STOPBAND_BEGIN, 1.0) : 1.0;

            char buf[256];
            sprintf_s(buf, "Custom (Precision: %.0f bits, Phase: %.1f%%, Passband End: %.3f, Stopband Begin: %.3f, DoublePrec: %s)",
                q_spec.precision, q_spec.phase_response, q_spec.passband_end, q_spec.stopband_begin,
                (doublePrec ? "Yes" : "No"));
            debugQualityName = buf;
        }
        soxr_error_t err;
        this->resampler = soxr_create(
            buffer->SampleRate(),
            this->configuredSampleRate,
            buffer->Channels(),
            &err,
            &io_spec,
            &q_spec,
            NULL
        );
        if (err) {
            LogError("Failed to create soxr resampler: " + std::string(soxr_strerror(err)));
            return false;
        }
        LogInfo("Created soxr resampler to resample " + std::to_string(buffer->SampleRate()) + 
                " Hz -> " + std::to_string(this->configuredSampleRate) + " Hz (Preset: " + debugQualityName + ")");

        // Pre-allocate the resample buffer to avoid heap allocations/zero-initialization overhead
        // in the real-time playback loop (Play()). We estimate the output size based on the resample ratio,
        // and add a generous safety margin (+ 256 frames).
        UINT32 inputFrames = buffer->Samples() / buffer->Channels();
        UINT32 expectedOutputFrames = (UINT32)((double)inputFrames * (double)this->configuredSampleRate / (double)buffer->SampleRate()) + 256;
        this->resampleBuffer.resize(expectedOutputFrames * buffer->Channels());
    }

    this->rate = buffer->SampleRate();

    // Write silence to let the DAC clock settle.
    // Rationale: When sample rates or formats change, the hardware DAC clock must re-lock to the new frequency.
    // If audio playback begins immediately, this re-locking process often produces an audible click, pop,
    // or brief hardware-induced stutter. Writing a short period of silence allows the DAC to stabilize.
    int settlingMs = ::prefs ? ::prefs->GetInt(PREF_DAC_SETTLING_MS, 0) : 0;
    if (settlingMs > 0 && rateChanged) {
        UINT32 silenceFramesNeeded = (UINT32)(this->configuredSampleRate * (settlingMs / 1000.0));
        UINT32 silenceFramesWritten = 0;

        while (silenceFramesWritten < silenceFramesNeeded) {
            UINT32 padding = 0;
            if (this->audioClient->GetCurrentPadding(&padding) == S_OK) {
                UINT32 available = this->outputBufferFrames - padding;
                if (available > 0) {
                    UINT32 toWrite = (std::min)(available, silenceFramesNeeded - silenceFramesWritten);
                    BYTE* data = nullptr;
                    if (this->renderClient->GetBuffer(toWrite, &data) == S_OK) {
                        ZeroMemory(data, toWrite * wf.Format.nBlockAlign);
                        this->renderClient->ReleaseBuffer(toWrite, 0);
                        silenceFramesWritten += toWrite;
                    }
                }
            }
            Sleep(10);
        }
    }

    double headroomDb = ::prefs ? ::prefs->GetDouble(PREF_SOXR_HEADROOM_DB, 0.0) : 0.0;
    this->headroomMultiplier = (float)std::pow(10.0, headroomDb / 20.0);

    return true;
}
