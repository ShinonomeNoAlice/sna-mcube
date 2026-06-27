#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <sstream>

#ifndef AUDCLNT_E_INVALID_DEVICE_PERIOD
#define AUDCLNT_E_INVALID_DEVICE_PERIOD ((HRESULT)0x88890020)
#endif
#ifndef AUDCLNT_E_INVALID_STREAM_FLAGS
#define AUDCLNT_E_INVALID_STREAM_FLAGS ((HRESULT)0x88890021)
#endif

// Zero-dependency GUID definitions to avoid linking/header issues
const GUID MY_KSDATAFORMAT_SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID MY_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// Map HRESULT values to readable, clean status strings (without AUDCLNT_E_ prefix)
std::string GetStatusString(HRESULT hr) {
    switch (hr) {
        case S_OK: return "OK";
        case AUDCLNT_E_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case AUDCLNT_E_ALREADY_INITIALIZED: return "ALREADY_INITIALIZED";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE: return "WRONG_ENDPOINT_TYPE";
        case AUDCLNT_E_DEVICE_INVALIDATED: return "DEVICE_INVALIDATED";
        case AUDCLNT_E_NOT_STOPPED: return "NOT_STOPPED";
        case AUDCLNT_E_BUFFER_TOO_LARGE: return "BUFFER_TOO_LARGE";
        case AUDCLNT_E_OUT_OF_ORDER: return "OUT_OF_ORDER";
        case AUDCLNT_E_UNSUPPORTED_FORMAT: return "UNSUPPORTED_FORMAT";
        case AUDCLNT_E_INVALID_SIZE: return "INVALID_SIZE";
        case AUDCLNT_E_DEVICE_IN_USE: return "DEVICE_IN_USE";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING: return "BUFFER_OPERATION_PENDING";
        case AUDCLNT_E_THREAD_NOT_REGISTERED: return "THREAD_NOT_REGISTERED";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "EXCLUSIVE_MODE_NOT_ALLOWED";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "ENDPOINT_CREATE_FAILED";
        case AUDCLNT_E_SERVICE_NOT_RUNNING: return "SERVICE_NOT_RUNNING";
        case AUDCLNT_E_EVENTHANDLE_NOT_SET: return "EVENTHANDLE_NOT_SET";
        case AUDCLNT_E_EXCLUSIVE_MODE_ONLY: return "EXCLUSIVE_MODE_ONLY";
        case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL: return "BUFDURATION_PERIOD_NOT_EQUAL";
        case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED: return "EVENTHANDLE_NOT_EXPECTED";
        case AUDCLNT_E_BUFFER_ERROR: return "BUFFER_ERROR";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return "BUFFER_SIZE_NOT_ALIGNED";
        case AUDCLNT_E_INVALID_DEVICE_PERIOD: return "INVALID_DEVICE_PERIOD";
        case AUDCLNT_E_INVALID_STREAM_FLAGS: return "INVALID_STREAM_FLAGS";
        case E_INVALIDARG: return "INVALIDARG";
        case E_POINTER: return "POINTER";
        case E_OUTOFMEMORY: return "OUTOFMEMORY";
        case E_FAIL: return "FAIL";
        default: {
            char buf[32];
            sprintf_s(buf, "0x%08X", hr);
            return std::string(buf);
        }
    }
}

// Table Formatter for printing perfectly-aligned Markdown tables with visually merged double headers
class TableFormatter {
public:
    TableFormatter(const std::vector<std::string>& headers1, const std::vector<std::string>& headers2)
        : headers1_(headers1), headers2_(headers2) {}

    void AddRow(const std::vector<std::string>& row) {
        rows_.push_back(row);
    }

    void Print() const {
        size_t cols = headers1_.size();
        std::vector<size_t> col_widths(cols, 0);

        for (size_t i = 0; i < cols; ++i) {
            // Skip combined header columns (5, 6, 7, 8) in the first pass to let them shrink.
            // They will be adjusted to fit the merged header width later if needed.
            if (i != 5 && i != 6 && i != 7 && i != 8) {
                col_widths[i] = (std::max)(col_widths[i], headers1_[i].size());
            }
            if (i < headers2_.size()) {
                col_widths[i] = (std::max)(col_widths[i], headers2_[i].size());
            }
        }
        for (const auto& row : rows_) {
            if (row.size() == 1 && row[0] == "---") continue;
            for (size_t i = 0; i < cols && i < row.size(); ++i) {
                col_widths[i] = (std::max)(col_widths[i], row[i].size());
            }
        }

        // Adjust columns 5 and 6 if their combined width (including spaces and separator) is smaller than "Req Buffer"
        if (cols > 6) {
            size_t req_buf_width = headers1_[5].size();
            size_t current_buf_width = col_widths[5] + col_widths[6] + 3;
            if (current_buf_width < req_buf_width) {
                col_widths[5] += (req_buf_width - current_buf_width);
            }
        }
        // Adjust columns 7 and 8 if their combined width (including spaces and separator) is smaller than "Req Period"
        if (cols > 8) {
            size_t req_per_width = headers1_[7].size();
            size_t current_per_width = col_widths[7] + col_widths[8] + 3;
            if (current_per_width < req_per_width) {
                col_widths[7] += (req_per_width - current_per_width);
            }
        }

        // Calculate total table width
        size_t total_width = 1;
        for (size_t w : col_widths) {
            total_width += w + 3;
        }

        // Print header 1 (with visually merged cells for Req Buffer & Req Period)
        std::cout << "|";
        for (size_t i = 0; i < cols; ) {
            if (i == 5) { // Req Buffer (HNS & ms combined)
                size_t combined_width = col_widths[5] + col_widths[6] + 3;
                std::cout << " " << PadRight(headers1_[5], combined_width) << " |";
                i += 2;
            } else if (i == 7) { // Req Period (HNS & ms combined)
                size_t combined_width = col_widths[7] + col_widths[8] + 3;
                std::cout << " " << PadRight(headers1_[7], combined_width) << " |";
                i += 2;
            } else {
                std::cout << " " << PadRight(headers1_[i], col_widths[i]) << " |";
                i += 1;
            }
        }
        std::cout << "\n";

        // Print header 2 (unmerged subheaders HNS and ms)
        std::cout << "|";
        for (size_t i = 0; i < cols; ++i) {
            std::string val = (i < headers2_.size()) ? headers2_[i] : "";
            std::cout << " " << PadRight(val, col_widths[i]) << " |";
        }
        std::cout << "\n";

        // Print separator
        std::cout << "|";
        for (size_t i = 0; i < cols; ++i) {
            std::cout << " " << std::string(col_widths[i], '-') << " |";
        }
        std::cout << "\n";

        // Print rows
        for (const auto& row : rows_) {
            if (row.size() == 1 && row[0] == "---") {
                std::cout << std::string(total_width, '-') << "\n";
                continue;
            }
            std::cout << "|";
            for (size_t i = 0; i < cols; ++i) {
                std::string val = (i < row.size()) ? row[i] : "";
                std::cout << " " << PadRight(val, col_widths[i]) << " |";
            }
            std::cout << "\n";
        }
    }

    void PrintCSV() const {
        size_t cols = headers1_.size();
        for (size_t i = 0; i < cols; ++i) {
            std::string headerName = headers1_[i];
            if (i < headers2_.size() && !headers2_[i].empty()) {
                headerName += " (" + headers2_[i] + ")";
            }
            std::cout << EscapeCSV(headerName);
            if (i + 1 < cols) std::cout << ",";
        }
        std::cout << "\n";

        for (const auto& row : rows_) {
            if (row.size() == 1 && row[0] == "---") continue;
            for (size_t i = 0; i < cols && i < row.size(); ++i) {
                std::cout << EscapeCSV(row[i]);
                if (i + 1 < cols) std::cout << ",";
            }
            std::cout << "\n";
        }
    }

private:
    static std::string EscapeCSV(const std::string& field) {
        bool needQuotes = false;
        if (field.find(',') != std::string::npos ||
            field.find('"') != std::string::npos ||
            field.find('\n') != std::string::npos) {
            needQuotes = true;
        }
        if (!needQuotes) return field;
        std::string res = "\"";
        for (char c : field) {
            if (c == '"') {
                res += "\"\"";
            } else {
                res += c;
            }
        }
        res += "\"";
        return res;
    }
    static std::string PadRight(const std::string& str, size_t width) {
        if (str.size() >= width) return str;
        return str + std::string(width - str.size(), ' ');
    }

    std::vector<std::string> headers1_;
    std::vector<std::string> headers2_;
    std::vector<std::vector<std::string>> rows_;
};

// Formats REFERENCE_TIME (100-nanosecond units) to a readable string
std::string FormatHns(REFERENCE_TIME hns) {
    char buf[64];
    sprintf_s(buf, "%lld (%.1f ms)", hns, hns / 10000.0);
    return std::string(buf);
}

// Formats REFERENCE_TIME (100-nanosecond units) as HNS string value
std::string FormatHnsValue(REFERENCE_TIME hns) {
    return std::to_string(hns);
}

// Formats REFERENCE_TIME (100-nanosecond units) as ms string value (1 decimal place)
std::string FormatMsValue(REFERENCE_TIME hns) {
    char buf[32];
    sprintf_s(buf, "%.1f", hns / 10000.0);
    return std::string(buf);
}

struct FormatDef {
    std::string type;
    int validBits;
    int containerBits;
    DWORD sampleRate;
    GUID subFormat;
    bool isAdvanced;
};

struct TestCase {
    std::string name;
    DWORD flags;
    REFERENCE_TIME bufferDuration;
    REFERENCE_TIME periodicity;
    bool useDefPeriodBuffer;
    bool useMinPeriodBuffer;
    bool useDefPeriodicity;
    bool useMinPeriodicity;
    bool useScaled;
};

// Performs stream initialization testing including the "Alignment Dance"
HRESULT TestInitializeAndDance(
    IMMDevice* device,
    const WAVEFORMATEX* format,
    DWORD flags,
    REFERENCE_TIME requestedBuffer,
    REFERENCE_TIME requestedPeriod,
    DWORD targetSampleRate,
    bool& outAlignedDanceTriggered,
    REFERENCE_TIME& outAlignedDuration,
    UINT32& outAlignedFrames
) {
    outAlignedDanceTriggered = false;
    outAlignedDuration = 0;
    outAlignedFrames = 0;

    IAudioClient* client = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
    if (FAILED(hr)) {
        return hr;
    }

    hr = client->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        flags,
        requestedBuffer,
        requestedPeriod,
        format,
        NULL
    );

    // Implement the Alignment Dance if AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED is returned
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        outAlignedDanceTriggered = true;
        UINT32 alignedFrames = 0;
        HRESULT hrSize = client->GetBufferSize(&alignedFrames);
        if (SUCCEEDED(hrSize)) {
            outAlignedFrames = alignedFrames;
            double calc = (10000000.0 * alignedFrames) / targetSampleRate;
            REFERENCE_TIME alignedDuration = static_cast<REFERENCE_TIME>(calc + 0.5);
            outAlignedDuration = alignedDuration;

            client->Release();
            client = nullptr;

            hrSize = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
            if (SUCCEEDED(hrSize)) {
                REFERENCE_TIME newBuffer = alignedDuration;
                REFERENCE_TIME newPeriod = requestedPeriod;
                if (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
                    newPeriod = alignedDuration;
                }

                hr = client->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    flags,
                    newBuffer,
                    newPeriod,
                    format,
                    NULL
                );
            } else {
                hr = hrSize;
            }
        }
    }

    if (client) {
        if (SUCCEEDED(hr)) {
            client->Reset();
        }
        client->Release();
    }

    return hr;
}

// Lists active render endpoints and prompts the user or processes index choice
IMMDevice* GetDeviceByIndexOrPrompt(IMMDeviceEnumerator* enumerator, int requestedIndex, bool listDevicesOnly, bool runCSV) {
    std::ostream& out = runCSV ? std::cerr : std::cout;
    std::wostream& wout = runCSV ? std::wcerr : std::wcout;
    IMMDeviceCollection* deviceCollection = nullptr;
    UINT deviceCount = 0;
    IMMDevice* selectedDevice = nullptr;

    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (FAILED(hr)) {
        out << "Failed to enumerate audio endpoints.\n";
        return nullptr;
    }

    if (FAILED(deviceCollection->GetCount(&deviceCount))) {
        deviceCollection->Release();
        return nullptr;
    }

    out << "Available Audio Render Devices:\n";
    for (UINT i = 0; i < deviceCount; i++) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(deviceCollection->Item(i, &dev))) {
            IPropertyStore* propStore = nullptr;
            std::wstring friendlyNameStr = L"Unknown Device";
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &propStore))) {
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);
                if (SUCCEEDED(propStore->GetValue(PKEY_Device_FriendlyName, &friendlyName))) {
                    friendlyNameStr = friendlyName.pwszVal;
                    PropVariantClear(&friendlyName);
                }
                propStore->Release();
            }
            wout << L"  Device [" << i << L"]: " << friendlyNameStr << L"\n";
            dev->Release();
        }
    }
    out << "====================================\n\n";

    if (listDevicesOnly) {
        deviceCollection->Release();
        return nullptr;
    }

    int finalIndex = requestedIndex;
    if (finalIndex < 0) {
        if (deviceCount > 0) {
            out << "Enter device index to test (0 to " << (deviceCount - 1) << ", or press Enter for default): ";
            std::string input;
            std::getline(std::cin, input);
            if (!input.empty()) {
                try {
                    finalIndex = std::stoi(input);
                } catch (...) {
                    finalIndex = -1;
                }
            }
        }
    }

    if (finalIndex >= 0 && finalIndex < (int)deviceCount) {
        hr = deviceCollection->Item(finalIndex, &selectedDevice);
        if (FAILED(hr)) {
            out << "Failed to get selected device from collection: " << GetStatusString(hr) << "\n";
        }
    }

    if (!selectedDevice) {
        out << "Using default multimedia device.\n";
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &selectedDevice);
        if (FAILED(hr)) {
            out << "Failed to get default endpoint: " << GetStatusString(hr) << "\n";
        }
    }

    deviceCollection->Release();
    return selectedDevice;
}

// Generates formats list depending on runAdvanced flag
std::vector<FormatDef> GenerateFormats(bool runAdvanced) {
    std::vector<FormatDef> formats;

    struct FormatBase {
        std::string type;
        int validBits;
        int containerBits;
        GUID subFormat;
        bool isAdvanced;
    };

    std::vector<FormatBase> formatBases = {
        // Standard formats
        { "PCM", 16, 16, MY_KSDATAFORMAT_SUBTYPE_PCM, false },
        { "PCM", 16, 24, MY_KSDATAFORMAT_SUBTYPE_PCM, false },
        { "PCM", 24, 24, MY_KSDATAFORMAT_SUBTYPE_PCM, false },
        { "PCM", 24, 32, MY_KSDATAFORMAT_SUBTYPE_PCM, false },
        { "PCM", 32, 32, MY_KSDATAFORMAT_SUBTYPE_PCM, false },
        { "Float", 32, 32, MY_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, false }
    };

    if (runAdvanced) {
        // Advanced formats
        std::vector<FormatBase> advBases = {
            { "PCM", 8, 8, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 18, 18, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 18, 24, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 18, 32, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 20, 20, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 20, 24, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 20, 32, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 22, 22, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 22, 24, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 22, 32, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "PCM", 64, 64, MY_KSDATAFORMAT_SUBTYPE_PCM, true },
            { "Float", 64, 64, MY_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, true }
        };
        formatBases.insert(formatBases.end(), advBases.begin(), advBases.end());
    }

    std::vector<DWORD> stdRates = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
    std::vector<DWORD> advRates;
    if (runAdvanced) {
        advRates = { 705600, 768000, 1411200, 1536000 };
    }

    for (const auto& base : formatBases) {
        // Run standard rates
        for (DWORD rate : stdRates) {
            formats.push_back({ base.type, base.validBits, base.containerBits, rate, base.subFormat, base.isAdvanced });
        }
        // If advanced run, also run advanced rates
        for (DWORD rate : advRates) {
            formats.push_back({ base.type, base.validBits, base.containerBits, rate, base.subFormat, true });
        }
    }

    return formats;
}

void PrintHelp() {
    std::cout << "Usage: test_wasapi [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help             Show this help message and exit\n";
    std::cout << "  -l, --list-devices     List all active audio render devices and exit\n";
    std::cout << "  -d, --device <index>   Test the device at the specified index\n";
    std::cout << "  -a, --advanced         Run advanced format and sample rate tests\n";
    std::cout << "  -c, --csv              Output the capability matrix in CSV format\n\n";
}

int main(int argc, char* argv[]) {
    // Parse arguments
    int deviceIndex = -1;
    bool runAdvanced = false;
    bool listDevicesOnly = false;
    bool runCSV = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelp();
            return 0;
        } else if (arg == "-l" || arg == "--list-devices") {
            listDevicesOnly = true;
        } else if (arg == "-a" || arg == "--advanced") {
            runAdvanced = true;
        } else if (arg == "-c" || arg == "--csv") {
            runCSV = true;
        } else if ((arg == "-d" || arg == "--device") && i + 1 < argc) {
            try {
                deviceIndex = std::stoi(argv[++i]);
            } catch (...) {
                std::cout << "Invalid device index specified.\n";
                return 1;
            }
        }
    }

    if (!runCSV) {
        std::cout << "WASAPI Exclusive Mode Capability Matrix Test Suite\n";
        std::cout << "==================================================\n";
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cout << "CoInitializeEx failed: " << GetStatusString(hr) << "\n";
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
        std::cout << "Failed to create MMDeviceEnumerator: " << GetStatusString(hr) << "\n";
        CoUninitialize();
        return 1;
    }

    IMMDevice* selectedDevice = GetDeviceByIndexOrPrompt(enumerator, deviceIndex, listDevicesOnly, runCSV);
    if (!selectedDevice) {
        enumerator->Release();
        CoUninitialize();
        return 0;
    }

    // Print friendly name of the selected device
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(selectedDevice->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &friendlyName))) {
            if (runCSV) {
                std::wcerr << L"Testing Device: " << friendlyName.pwszVal << L"\n";
            } else {
                std::wcout << L"Testing Device: " << friendlyName.pwszVal << L"\n";
            }
            PropVariantClear(&friendlyName);
        }
        props->Release();
    }

    // Get startup timings
    IAudioClient* startupClient = nullptr;
    hr = selectedDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&startupClient);
    if (FAILED(hr)) {
        std::cerr << "Failed to activate startup IAudioClient: " << GetStatusString(hr) << "\n";
        selectedDevice->Release();
        enumerator->Release();
        CoUninitialize();
        return 1;
    }

    REFERENCE_TIME minPeriodStartup = 0;
    REFERENCE_TIME defPeriodStartup = 0;
    hr = startupClient->GetDevicePeriod(&defPeriodStartup, &minPeriodStartup);
    if (FAILED(hr)) {
        std::cerr << "Failed to query startup GetDevicePeriod: " << GetStatusString(hr) << "\n";
        startupClient->Release();
        selectedDevice->Release();
        enumerator->Release();
        CoUninitialize();
        return 1;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = startupClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        std::cerr << "Failed to query startup GetMixFormat: " << GetStatusString(hr) << "\n";
        startupClient->Release();
        selectedDevice->Release();
        enumerator->Release();
        CoUninitialize();
        return 1;
    }

    DWORD startupSampleRate = mixFormat->nSamplesPerSec;
    if (runCSV) {
        std::cerr << "Device Startup Mix Format:\n";
        std::cerr << "  Sample Rate: " << startupSampleRate << " Hz\n";
        std::cerr << "  Bits Per Sample: " << mixFormat->wBitsPerSample << "\n";
        std::cerr << "  Channels: " << mixFormat->nChannels << "\n";
        std::cerr << "Device Startup Periodicity:\n";
        std::cerr << "  Default Period: " << FormatHns(defPeriodStartup) << "\n";
        std::cerr << "  Minimum Period: " << FormatHns(minPeriodStartup) << "\n\n";
    } else {
        std::cout << "Device Startup Mix Format:\n";
        std::cout << "  Sample Rate: " << startupSampleRate << " Hz\n";
        std::cout << "  Bits Per Sample: " << mixFormat->wBitsPerSample << "\n";
        std::cout << "  Channels: " << mixFormat->nChannels << "\n";
        std::cout << "Device Startup Periodicity:\n";
        std::cout << "  Default Period: " << FormatHns(defPeriodStartup) << "\n";
        std::cout << "  Minimum Period: " << FormatHns(minPeriodStartup) << "\n\n";
    }

    CoTaskMemFree(mixFormat);
    startupClient->Release();

    // Generate formats to test
    std::vector<FormatDef> formatsToTest = GenerateFormats(runAdvanced);

    // Set up test cases testing both scaled and unscaled Windows periods
    std::vector<TestCase> testCases = {
        // Polling (Unscaled)
        { "Polling (Def/Def - Unscaled)", 0, 0, 0, true, false, true, false, false },
        { "Polling (Def/Min - Unscaled)", 0, 0, 0, true, false, false, true, false },
        { "Polling (Min/Def - Unscaled)", 0, 0, 0, false, true, true, false, false },
        { "Polling (Min/Min - Unscaled)", 0, 0, 0, false, true, false, true, false },

        // Polling (Scaled)
        { "Polling (Def/Def - Scaled)", 0, 0, 0, true, false, true, false, true },
        { "Polling (Def/Min - Scaled)", 0, 0, 0, true, false, false, true, true },
        { "Polling (Min/Def - Scaled)", 0, 0, 0, false, true, true, false, true },
        { "Polling (Min/Min - Scaled)", 0, 0, 0, false, true, false, true, true },

        // Polling (Arbitrary & Zero - Unscaled)
        { "Polling (10ms/Min - Unscaled)",  0, 100000, 0, false, false, false, true, false },
        { "Polling (20ms/Min - Unscaled)",  0, 200000, 0, false, false, false, true, false },
        { "Polling (100ms/Def - Unscaled)", 0, 1000000, 0, false, false, true, false, false },
        { "Polling (200ms/Def - Unscaled)", 0, 2000000, 0, false, false, true, false, false },
        { "Polling (Def/0 - Unscaled)",     0, 0, 0, true, false, false, false, false },

        // Polling (Arbitrary & Zero - Scaled)
        { "Polling (10ms/Min - Scaled)",  0, 100000, 0, false, false, false, true, true },
        { "Polling (20ms/Min - Scaled)",  0, 200000, 0, false, false, false, true, true },
        { "Polling (100ms/Def - Scaled)", 0, 1000000, 0, false, false, true, false, true },
        { "Polling (200ms/Def - Scaled)", 0, 2000000, 0, false, false, true, false, true },
        { "Polling (100ms/0)",            0, 1000000, 0, false, false, false, false, false },
        { "Polling (Def/0 - Scaled)",      0, 0, 0, true, false, false, false, true },

        // Event-driven (Unscaled)
        { "Event-driven (Def/Def - Unscaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, true, false, true, false, false },
        { "Event-driven (Def/Min - Unscaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, true, false, false, true, false },
        { "Event-driven (Min/Def - Unscaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, false, true, true, false, false },
        { "Event-driven (Min/Min - Unscaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, false, true, false, true, false },

        // Event-driven (Scaled)
        { "Event-driven (Def/Def - Scaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, true, false, true, false, true },
        { "Event-driven (Def/Min - Scaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, true, false, false, true, true },
        { "Event-driven (Min/Def - Scaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, false, true, true, false, true },
        { "Event-driven (Min/Min - Scaled)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, false, true, false, true, true },

        // Event-driven (Arbitrary)
        { "Event-driven (10ms/10ms)", AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 100000, false, false, false, false, false }
    };

    // Prepare table formatter with merged Excel-style headers
    TableFormatter table(
        { "Format", "Valid Bits", "Container Bits", "Sample Rate (Hz)", "Mode", "Req Buffer", "Req Buffer", "Req Period", "Req Period", "Status", "Buffer Source", "Period Source" },
        { "", "", "", "", "", "HNS", "ms", "HNS", "ms", "", "", "" }
    );

    std::string prevType = "";
    int prevValidBits = 0;
    int prevContainerBits = 0;
    DWORD prevSampleRate = 0;
    std::string prevMode = "";

    for (const auto& fmt : formatsToTest) {
        WAVEFORMATEXTENSIBLE wfe;
        ZeroMemory(&wfe, sizeof(WAVEFORMATEXTENSIBLE));
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.nChannels = 2; // Stereo testing
        wfe.Format.nSamplesPerSec = fmt.sampleRate;
        wfe.Format.wBitsPerSample = fmt.containerBits;
        wfe.Format.nBlockAlign = ((wfe.Format.wBitsPerSample + 7) / 8) * wfe.Format.nChannels;
        wfe.Format.nAvgBytesPerSec = wfe.Format.nSamplesPerSec * wfe.Format.nBlockAlign;
        wfe.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfe.Samples.wValidBitsPerSample = fmt.validBits;
        wfe.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfe.SubFormat = fmt.subFormat;

        // Query support
        IAudioClient* queryClient = nullptr;
        hr = selectedDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&queryClient);
        HRESULT supportHr = E_FAIL;
        if (SUCCEEDED(hr)) {
            supportHr = queryClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wfe, NULL);
            queryClient->Release();
        } else {
            supportHr = hr;
        }

        if (supportHr != S_OK) {
            std::string modeStr = "IsFormatSupported";
            
            // Check major parameter switch for separator
            if (!prevType.empty()) {
                if (fmt.type != prevType ||
                    fmt.validBits != prevValidBits ||
                    fmt.containerBits != prevContainerBits ||
                    fmt.sampleRate != prevSampleRate ||
                    modeStr != prevMode) {
                    table.AddRow({"---"});
                }
            }
            prevType = fmt.type;
            prevValidBits = fmt.validBits;
            prevContainerBits = fmt.containerBits;
            prevSampleRate = fmt.sampleRate;
            prevMode = modeStr;

            // Unsupported format - output single row
            table.AddRow({
                fmt.type,
                std::to_string(fmt.validBits),
                std::to_string(fmt.containerBits),
                std::to_string(fmt.sampleRate),
                modeStr,
                "-",
                "-",
                "-",
                "-",
                GetStatusString(supportHr),
                "-",
                "-"
            });
            continue;
        }

        // Scale timing variables for the target sample rate
        REFERENCE_TIME hnsMinPeriod = static_cast<REFERENCE_TIME>(minPeriodStartup * (static_cast<double>(startupSampleRate) / fmt.sampleRate) + 0.5);
        REFERENCE_TIME hnsDefPeriod = static_cast<REFERENCE_TIME>(defPeriodStartup * (static_cast<double>(startupSampleRate) / fmt.sampleRate) + 0.5);
        if (hnsMinPeriod < 1) hnsMinPeriod = 1;
        if (hnsDefPeriod < 1) hnsDefPeriod = 1;

        // Run the initialization tests
        for (const auto& tc : testCases) {
            REFERENCE_TIME requestedBuffer = tc.bufferDuration;
            if (tc.useDefPeriodBuffer) {
                requestedBuffer = tc.useScaled ? hnsDefPeriod : defPeriodStartup;
            } else if (tc.useMinPeriodBuffer) {
                requestedBuffer = tc.useScaled ? hnsMinPeriod : minPeriodStartup;
            }

            REFERENCE_TIME requestedPeriod = tc.periodicity;
            if (tc.useDefPeriodicity) {
                requestedPeriod = tc.useScaled ? hnsDefPeriod : defPeriodStartup;
            } else if (tc.useMinPeriodicity) {
                requestedPeriod = tc.useScaled ? hnsMinPeriod : minPeriodStartup;
            }

            bool alignedDanceTriggered = false;
            REFERENCE_TIME alignedDuration = 0;
            UINT32 alignedFrames = 0;

            HRESULT testHr = TestInitializeAndDance(
                selectedDevice,
                (WAVEFORMATEX*)&wfe,
                tc.flags,
                requestedBuffer,
                requestedPeriod,
                fmt.sampleRate,
                alignedDanceTriggered,
                alignedDuration,
                alignedFrames
            );

            std::string statusStr = GetStatusString(testHr);
            if (testHr == S_OK) {
                if (alignedDanceTriggered) {
                    statusStr = "OK (Aligned: " + std::to_string(alignedFrames) + " frames)";
                } else {
                    statusStr = "OK";
                }
            }

            std::string modeStr = (tc.flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) ? "Event" : "Polling";

            // Check major parameter switch for separator
            if (!prevType.empty()) {
                if (fmt.type != prevType ||
                    fmt.validBits != prevValidBits ||
                    fmt.containerBits != prevContainerBits ||
                    fmt.sampleRate != prevSampleRate ||
                    modeStr != prevMode) {
                    table.AddRow({"---"});
                }
            }
            prevType = fmt.type;
            prevValidBits = fmt.validBits;
            prevContainerBits = fmt.containerBits;
            prevSampleRate = fmt.sampleRate;
            prevMode = modeStr;

            // Determine Timing Source info
            std::string bufferSource;
            if (alignedDanceTriggered && testHr == S_OK) {
                bufferSource = "Aligned via GetBufferSize";
            } else if (tc.useDefPeriodBuffer) {
                bufferSource = tc.useScaled ? "Scaled Win Default" : "Unscaled Win Default";
            } else if (tc.useMinPeriodBuffer) {
                bufferSource = tc.useScaled ? "Scaled Win Minimum" : "Unscaled Win Minimum";
            } else {
                bufferSource = "Arbitrary (" + std::to_string(tc.bufferDuration / 10000) + "ms)";
            }

            std::string periodSource;
            if (alignedDanceTriggered && testHr == S_OK && (tc.flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) {
                periodSource = "Aligned via GetBufferSize";
            } else if (tc.useDefPeriodicity) {
                periodSource = tc.useScaled ? "Scaled Win Default" : "Unscaled Win Default";
            } else if (tc.useMinPeriodicity) {
                periodSource = tc.useScaled ? "Scaled Win Minimum" : "Unscaled Win Minimum";
            } else if (requestedPeriod == 0) {
                periodSource = "Arbitrary (0)";
            } else {
                periodSource = "Arbitrary (" + std::to_string(requestedPeriod / 10000) + "ms)";
            }

            table.AddRow({
                fmt.type,
                std::to_string(fmt.validBits),
                std::to_string(fmt.containerBits),
                std::to_string(fmt.sampleRate),
                modeStr,
                FormatHnsValue(requestedBuffer),
                FormatMsValue(requestedBuffer),
                FormatHnsValue(requestedPeriod),
                FormatMsValue(requestedPeriod),
                statusStr,
                bufferSource,
                periodSource
            });
        }
    }

    if (runCSV) {
        table.PrintCSV();
    } else {
        std::cout << "Exclusive Mode Capability Matrix:\n\n";
        table.Print();
        std::cout << "\n";
    }

    selectedDevice->Release();
    enumerator->Release();
    CoUninitialize();
    return 0;
}
