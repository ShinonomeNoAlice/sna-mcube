#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <string>
#include <vector>

#define EXIT_ON_ERROR(hr) if (FAILED(hr)) { std::cout << "Error: " << hr << " at line " << __LINE__ << "\n"; goto Exit; }

std::string HResultToString(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08X", hr);
    return std::string(buf);
}

void TestInitialize(IAudioClient* client, const WAVEFORMATEX* format, DWORD flags, REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity, const std::string& label) {
    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        flags,
        bufferDuration,
        periodicity,
        format,
        NULL
    );
    std::cout << "  " << label << " -> " << HResultToString(hr);
    if (hr == S_OK) {
        std::cout << " (SUCCESS)";
        client->Reset();
    }
    std::cout << "\n";
}

int main() {
    std::cout << "WASAPI Exclusive Mode Diagnosis Tool\n";
    std::cout << "====================================\n";

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cout << "CoInitializeEx failed\n";
        return 1;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );
    if (FAILED(hr)) {
        std::cout << "Failed to create MMDeviceEnumerator\n";
        return 1;
    }

    // Query and list all active audio render devices so you can find your DAC's device ID.
    IMMDeviceCollection* deviceCollection = nullptr;
    UINT deviceCount = 0;
    std::vector<std::wstring> deviceIds;

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(deviceCollection->GetCount(&deviceCount))) {
            std::cout << "Available Audio Render Devices & IDs:\n";
            for (UINT i = 0; i < deviceCount; i++) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(deviceCollection->Item(i, &dev))) {
                    LPWSTR devIdPtr = nullptr;
                    if (SUCCEEDED(dev->GetId(&devIdPtr))) {
                        deviceIds.push_back(devIdPtr);
                        std::wcout << L"  Device [" << i << L"]: ID: " << devIdPtr << L"\n";
                        IPropertyStore* propStore = nullptr;
                        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &propStore))) {
                            PROPVARIANT friendlyName;
                            PropVariantInit(&friendlyName);
                            if (SUCCEEDED(propStore->GetValue(PKEY_Device_FriendlyName, &friendlyName))) {
                                std::wcout << L"            Name: " << friendlyName.pwszVal << L"\n";
                                PropVariantClear(&friendlyName);
                            }
                            propStore->Release();
                        }
                        CoTaskMemFree(devIdPtr);
                    } else {
                        deviceIds.push_back(L"");
                    }
                    dev->Release();
                }
            }
            std::cout << "====================================\n\n";
        }
    }

    IMMDevice* device = nullptr;
    int selectedIndex = -1;
    if (deviceCount > 0) {
        std::cout << "Enter device index to test (0 to " << (deviceCount - 1) << ", or press Enter for default): ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty()) {
            try {
                selectedIndex = std::stoi(input);
            } catch (...) {
                selectedIndex = -1;
            }
        }
    }

    if (selectedIndex >= 0 && selectedIndex < (int)deviceCount) {
        hr = deviceCollection->Item(selectedIndex, &device);
        if (FAILED(hr)) {
            std::cout << "Failed to get selected device from collection: " << HResultToString(hr) << "\n";
        }
    }

    if (deviceCollection) {
        deviceCollection->Release();
    }

    if (!device) {
        // Fallback to default multimedia device
        std::cout << "Using default multimedia device.\n";
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(hr)) {
            std::cout << "Failed to get default endpoint: " << HResultToString(hr) << "\n";
            enumerator->Release();
            return 1;
        }
    }

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &friendlyName))) {
            std::wcout << L"Device Friendly Name: " << friendlyName.pwszVal << L"\n";
            PropVariantClear(&friendlyName);
        }
        props->Release();
    }

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
    if (FAILED(hr)) {
        std::cout << "Failed to activate IAudioClient: " << HResultToString(hr) << "\n";
        device->Release();
        enumerator->Release();
        return 1;
    }

    REFERENCE_TIME defPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    hr = audioClient->GetDevicePeriod(&defPeriod, &minPeriod);
    if (SUCCEEDED(hr)) {
        std::cout << "GetDevicePeriod:\n";
        std::cout << "  Default Period: " << defPeriod << " hns (" << (defPeriod / 10000.0) << " ms)\n";
        std::cout << "  Minimum Period: " << minPeriod << " hns (" << (minPeriod / 10000.0) << " ms)\n";
    } else {
        std::cout << "GetDevicePeriod failed\n";
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (SUCCEEDED(hr)) {
        std::cout << "GetMixFormat:\n";
        std::cout << "  FormatTag: " << mixFormat->wFormatTag << "\n";
        std::cout << "  Channels: " << mixFormat->nChannels << "\n";
        std::cout << "  SamplesPerSec: " << mixFormat->nSamplesPerSec << "\n";
        std::cout << "  BitsPerSample: " << mixFormat->wBitsPerSample << "\n";
        std::cout << "  cbSize: " << mixFormat->cbSize << "\n";
    }

    // Standard formats to test: 44100 Hz stereo 32-bit float, 44100 Hz stereo 16-bit PCM, 48000 Hz versions
    struct FormatTest {
        std::string label;
        WORD bitsPerSample;
        WORD validBits;
        DWORD sampleRate;
        GUID subFormat;
    };

    std::vector<FormatTest> formatsToTest = {
        { "44.1kHz Stereo IEEE Float (32-bit)", 32, 32, 44100, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT },
        { "44.1kHz Stereo PCM 16-bit", 16, 16, 44100, KSDATAFORMAT_SUBTYPE_PCM },
        { "44.1kHz Stereo PCM 24-bit (packed)", 24, 24, 44100, KSDATAFORMAT_SUBTYPE_PCM },
        { "44.1kHz Stereo PCM 24-in-32-bit", 32, 24, 44100, KSDATAFORMAT_SUBTYPE_PCM },
        { "44.1kHz Stereo PCM 32-bit", 32, 32, 44100, KSDATAFORMAT_SUBTYPE_PCM },
        { "48kHz Stereo IEEE Float (32-bit)", 32, 32, 48000, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT },
        { "48kHz Stereo PCM 16-bit", 16, 16, 48000, KSDATAFORMAT_SUBTYPE_PCM },
        { "48kHz Stereo PCM 24-bit (packed)", 24, 24, 48000, KSDATAFORMAT_SUBTYPE_PCM },
        { "48kHz Stereo PCM 24-in-32-bit", 32, 24, 48000, KSDATAFORMAT_SUBTYPE_PCM },
        { "48kHz Stereo PCM 32-bit", 32, 32, 48000, KSDATAFORMAT_SUBTYPE_PCM }
    };

    for (const auto& fmt : formatsToTest) {
        WAVEFORMATEXTENSIBLE wfe;
        ZeroMemory(&wfe, sizeof(WAVEFORMATEXTENSIBLE));
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.nChannels = 2;
        wfe.Format.nSamplesPerSec = fmt.sampleRate;
        wfe.Format.wBitsPerSample = fmt.bitsPerSample;
        wfe.Format.nBlockAlign = (wfe.Format.wBitsPerSample / 8) * wfe.Format.nChannels;
        wfe.Format.nAvgBytesPerSec = wfe.Format.nSamplesPerSec * wfe.Format.nBlockAlign;
        wfe.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfe.Samples.wValidBitsPerSample = fmt.validBits;
        wfe.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfe.SubFormat = fmt.subFormat;

        std::cout << "\nTesting format: " << fmt.label << "\n";
        hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wfe, NULL);
        std::cout << "  IsFormatSupported (Exclusive) -> " << HResultToString(hr) << "\n";
        
        if (hr == S_OK) {
            // We need to activate fresh IAudioClient instances because once Initialize is called (even if it fails),
            // the IAudioClient instance might be in an undefined state or locked from subsequent Initialize calls.
            auto RunTest = [&](DWORD flags, REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity, const std::string& testLabel) {
                IAudioClient* testClient = nullptr;
                if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&testClient))) {
                    TestInitialize(testClient, (WAVEFORMATEX*)&wfe, flags, bufferDuration, periodicity, testLabel);
                    testClient->Release();
                } else {
                    std::cout << "  " << testLabel << " -> Failed to activate test client\n";
                }
            };

            // 1. Polling Mode: hnsPeriodicity matches defPeriod, bufferDuration varies
            RunTest(0, defPeriod, defPeriod, "Polling, Buffer=DefPeriod, Period=DefPeriod");
            RunTest(0, minPeriod, minPeriod, "Polling, Buffer=MinPeriod, Period=MinPeriod");
            RunTest(0, 1000000, defPeriod, "Polling, Buffer=100ms, Period=DefPeriod");
            RunTest(0, 2000000, defPeriod, "Polling, Buffer=200ms, Period=DefPeriod");
            RunTest(0, 1000000, minPeriod, "Polling, Buffer=100ms, Period=MinPeriod");
            RunTest(0, 2000000, minPeriod, "Polling, Buffer=200ms, Period=MinPeriod");
            
            // 2. Event-driven Mode: bufferDuration == periodicity
            RunTest(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, defPeriod, defPeriod, "Event-driven, Buffer=DefPeriod, Period=DefPeriod");
            RunTest(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, minPeriod, "Event-driven, Buffer=MinPeriod, Period=MinPeriod");

            // 3. Polling Mode: hnsPeriodicity set to 0 (which is invalid in some docs but let's test anyway)
            RunTest(0, 1000000, 0, "Polling, Buffer=100ms, Period=0");
            RunTest(0, defPeriod, 0, "Polling, Buffer=DefPeriod, Period=0");
        }
    }

Exit:
    if (mixFormat) CoTaskMemFree(mixFormat);
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    CoUninitialize();
    return 0;
}
