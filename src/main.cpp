#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tlhelp32.h>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

constexpr auto kReportInterval = std::chrono::milliseconds(250);
constexpr double kTargetDbfs = -16.0;
constexpr double kAttackMs = 50.0;
constexpr double kReleaseMs = 3000.0;
constexpr double kMaxGainDb = 20.0;
constexpr double kMinGainDb = -40.0;
constexpr double kSilenceGateDbfs = -60.0;
constexpr double kRerenderMonitorScale = 0.01;

HANDLE g_stopEvent = nullptr;

class HrError final : public std::runtime_error {
public:
    HrError(std::string message, HRESULT result)
        : std::runtime_error(std::move(message)), result_(result) {}

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

void check_hresult(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        throw HrError(std::string(operation), result);
    }
}

std::wstring format_hresult(HRESULT result) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring message = length == 0 ? L"Unknown error" : std::wstring(buffer, length);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class CoTaskMemWaveFormat final {
public:
    ~CoTaskMemWaveFormat() { CoTaskMemFree(value_); }
    CoTaskMemWaveFormat(const CoTaskMemWaveFormat&) = delete;
    CoTaskMemWaveFormat& operator=(const CoTaskMemWaveFormat&) = delete;
    CoTaskMemWaveFormat() = default;

    [[nodiscard]] WAVEFORMATEX** put() noexcept { return &value_; }
    [[nodiscard]] WAVEFORMATEX* get() const noexcept { return value_; }

private:
    WAVEFORMATEX* value_ = nullptr;
};

class ActivationHandler final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase,
                          IActivateAudioInterfaceCompletionHandler> {
public:
    void initialize(HANDLE completedEvent) noexcept { completedEvent_ = completedEvent; }

    STDMETHODIMP ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activationResult = E_UNEXPECTED;
        ComPtr<IUnknown> activatedInterface;
        result_ = operation->GetActivateResult(&activationResult, &activatedInterface);
        if (SUCCEEDED(result_)) {
            result_ = activationResult;
        }
        if (SUCCEEDED(result_)) {
            result_ = activatedInterface.As(&audioClient_);
        }
        SetEvent(completedEvent_);
        return S_OK;
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }
    [[nodiscard]] ComPtr<IAudioClient> audio_client() const noexcept {
        return audioClient_;
    }

private:
    HANDLE completedEvent_ = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> audioClient_;
};

BOOL WINAPI console_control_handler(DWORD controlType) {
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT) {
        if (g_stopEvent != nullptr) {
            SetEvent(g_stopEvent);
        }
        return TRUE;
    }
    return FALSE;
}

struct Options {
    DWORD processId = 0;
    std::optional<std::chrono::seconds> duration;
    bool rerender = false;
};

template <typename Integer>
std::optional<Integer> parse_integer(std::wstring_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string narrow;
    narrow.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>('0' + (character - L'0')));
    }
    Integer parsed{};
    const auto [end, error] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), parsed);
    if (error != std::errc{} || end != narrow.data() + narrow.size()) {
        return std::nullopt;
    }
    return parsed;
}

void print_usage() {
    std::wcout
        << L"Usage: levelmate-wasapi-probe.exe <root-pid> "
           L"[--duration <seconds>] [--rerender]\n"
        << L"AGC via session volume control. Target: "
        << static_cast<int>(kTargetDbfs) << L" dBFS, "
        << L"attack=" << static_cast<int>(kAttackMs) << L" ms, "
        << L"release=" << static_cast<int>(kReleaseMs) << L" ms.\n"
        << L"--rerender enables digital boost above the session-volume ceiling.\n";
}

std::optional<Options> parse_options(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        return std::nullopt;
    }

    const auto processId = parse_integer<unsigned long>(argv[1]);
    if (!processId || *processId == 0 || *processId > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }

    Options options{static_cast<DWORD>(*processId), std::nullopt, false};
    for (int index = 2; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--rerender") {
            if (options.rerender) {
                return std::nullopt;
            }
            options.rerender = true;
            continue;
        }
        if (argument != L"--duration" || options.duration || index + 1 >= argc) {
            return std::nullopt;
        }
        const auto seconds = parse_integer<long long>(argv[++index]);
        if (!seconds || *seconds <= 0) {
            return std::nullopt;
        }
        options.duration = std::chrono::seconds(*seconds);
    }
    return options;
}

void validate_process(DWORD processId) {
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "OpenProcess");
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "GetExitCodeProcess");
    }
    if (exitCode != STILL_ACTIVE) {
        throw std::runtime_error("The target process is not running");
    }

}

ComPtr<IAudioClient> activate_process_loopback(DWORD processId) {
    UniqueHandle completedEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!completedEvent) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent(activation)");
    }

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    if (!handler) {
        throw std::bad_alloc();
    }
    handler->initialize(completedEvent.get());

    AUDIOCLIENT_ACTIVATION_PARAMS activationParameters{};
    activationParameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParameters.ProcessLoopbackParams.TargetProcessId = processId;
    activationParameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT parameters{};
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activationParameters);
    parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParameters);

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    check_hresult(ActivateAudioInterfaceAsync(
                      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                      &parameters, handler.Get(), &operation),
                  "ActivateAudioInterfaceAsync");

    const DWORD waitResult = WaitForSingleObject(completedEvent.get(), INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "WaitForSingleObject(activation)");
    }
    check_hresult(handler->result(), "Process-loopback activation");
    return handler->audio_client();
}

std::vector<DWORD> get_process_tree(DWORD rootPid) {
    std::vector<DWORD> result;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return result;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snapshot.get(), &pe)) {
        return result;
    }

    std::vector<std::pair<DWORD, DWORD>> processes;
    do {
        processes.emplace_back(pe.th32ProcessID, pe.th32ParentProcessID);
    } while (Process32NextW(snapshot.get(), &pe));

    std::vector<DWORD> currentLevel = {rootPid};
    while (!currentLevel.empty()) {
        for (DWORD pid : currentLevel) {
            if (std::find(result.begin(), result.end(), pid) == result.end()) {
                result.push_back(pid);
            }
        }
        std::vector<DWORD> nextLevel;
        for (const auto& [pid, ppid] : processes) {
            if (std::find(currentLevel.begin(), currentLevel.end(), ppid) != currentLevel.end() &&
                std::find(result.begin(), result.end(), pid) == result.end()) {
                nextLevel.push_back(pid);
            }
        }
        currentLevel.swap(nextLevel);
    }

    return result;
}

struct SessionInfo {
    ComPtr<ISimpleAudioVolume> volume;
    DWORD processId;
    float originalVolume;
    std::wstring displayName;
};

std::vector<SessionInfo> enumerate_sessions(const std::vector<DWORD>& pids) {
    std::vector<SessionInfo> sessions;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return sessions;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) return sessions;

    ComPtr<IAudioSessionManager2> manager;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**>(manager.GetAddressOf()));
    if (FAILED(hr)) return sessions;

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    hr = manager->GetSessionEnumerator(&sessionEnum);
    if (FAILED(hr)) return sessions;

    int count = 0;
    check_hresult(sessionEnum->GetCount(&count), "IAudioSessionEnumerator::GetCount");

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(sessionEnum->GetSession(i, &ctrl))) continue;

        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&ctrl2)))) continue;

        DWORD sessionPid = 0;
        if (FAILED(ctrl2->GetProcessId(&sessionPid))) continue;

        if (std::find(pids.begin(), pids.end(), sessionPid) == pids.end()) continue;

        ComPtr<ISimpleAudioVolume> sv;
        if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&sv)))) continue;

        float originalVol = 0.0f;
        check_hresult(sv->GetMasterVolume(&originalVol),
                      "ISimpleAudioVolume::GetMasterVolume(original)");

        std::wstring dn;
        wchar_t* dnPtr = nullptr;
        if (SUCCEEDED(ctrl->GetDisplayName(&dnPtr)) && dnPtr != nullptr) {
            dn = dnPtr;
            CoTaskMemFree(dnPtr);
        }

        sessions.push_back({sv, sessionPid, originalVol, dn});
    }

    return sessions;
}

bool restore_sessions(std::vector<SessionInfo>& sessions) noexcept {
    bool restoredAll = true;
    for (auto& s : sessions) {
        const HRESULT result = s.volume->SetMasterVolume(s.originalVolume, nullptr);
        if (FAILED(result)) {
            restoredAll = false;
            std::wcerr << L"Warning: failed to restore volume for PID " << s.processId
                       << L": 0x" << std::hex << static_cast<unsigned long>(result)
                       << L" " << format_hresult(result) << L'\n';
        }
    }
    if (restoredAll) {
        sessions.clear();
    }
    return restoredAll;
}

class SessionRestoreGuard final {
public:
    explicit SessionRestoreGuard(std::vector<SessionInfo>& sessions) noexcept
        : sessions_(sessions) {}
    ~SessionRestoreGuard() { restore_sessions(sessions_); }

    SessionRestoreGuard(const SessionRestoreGuard&) = delete;
    SessionRestoreGuard& operator=(const SessionRestoreGuard&) = delete;

private:
    std::vector<SessionInfo>& sessions_;
};

enum class SampleEncoding { Float32, Pcm8, Pcm16, Pcm24, Pcm32 };

SampleEncoding sample_encoding(const WAVEFORMATEX& format) {
    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        constexpr WORD requiredExtensionSize = static_cast<WORD>(
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
        if (format.cbSize < requiredExtensionSize) {
            throw std::runtime_error("Invalid WAVE_FORMAT_EXTENSIBLE capture format");
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            tag = WAVE_FORMAT_PCM;
        } else {
            throw std::runtime_error("Unsupported extensible capture subformat");
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        return SampleEncoding::Float32;
    }
    if (tag == WAVE_FORMAT_PCM) {
        switch (format.wBitsPerSample) {
        case 8: return SampleEncoding::Pcm8;
        case 16: return SampleEncoding::Pcm16;
        case 24: return SampleEncoding::Pcm24;
        case 32: return SampleEncoding::Pcm32;
        default: break;
        }
    }
    throw std::runtime_error("Unsupported WASAPI sample encoding");
}

double decode_sample(const BYTE* sample, SampleEncoding encoding) {
    switch (encoding) {
    case SampleEncoding::Float32: {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(static_cast<double>(value))
                   ? std::clamp(static_cast<double>(value), -1.0, 1.0)
                   : 0.0;
    }
    case SampleEncoding::Pcm8:
        return (static_cast<double>(*sample) - 128.0) / 128.0;
    case SampleEncoding::Pcm16: {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    case SampleEncoding::Pcm24: {
        std::int32_t value = static_cast<std::int32_t>(sample[0]) |
                             (static_cast<std::int32_t>(sample[1]) << 8) |
                             (static_cast<std::int32_t>(sample[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<double>(value) / 8388608.0;
    }
    case SampleEncoding::Pcm32: {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    }
    return 0.0;
}

double to_dbfs(double amplitude) {
    return amplitude > 0.0 ? 20.0 * std::log10(amplitude)
                           : -std::numeric_limits<double>::infinity();
}

double from_dbfs(double dbfs) {
    return std::pow(10.0, dbfs / 20.0);
}

double calculate_smoothed_gain(double peakDb, double& smoothDb, double elapsedMs) {
    if (!std::isfinite(peakDb) || peakDb < kSilenceGateDbfs) {
        return 1.0;
    }

    const double timeConstant = peakDb > smoothDb ? kAttackMs : kReleaseMs;
    const double alpha = 1.0 - std::exp(-elapsedMs / timeConstant);
    smoothDb += alpha * (peakDb - smoothDb);
    return std::clamp(from_dbfs(kTargetDbfs) / from_dbfs(smoothDb),
                      from_dbfs(kMinGainDb), from_dbfs(kMaxGainDb));
}

double apply_peak_limiter(double gain, double peak) {
    return peak > 0.0 ? std::min(gain, from_dbfs(kTargetDbfs) / peak) : gain;
}

std::string dbfs_color(double dbfs) {
    if (dbfs < -40) return "\033[90m";
    if (dbfs < -20) return "\033[92m";
    if (dbfs < -10) return "\033[93m";
    if (dbfs < -6)  return "\033[91m";
    return "\033[97;101m";
}

constexpr int kMeterWidth = 20;

void print_meter_bar(double dbfs, int width) {
    double clamped = std::clamp(dbfs, -60.0, 0.0);
    int filled = static_cast<int>((clamped + 60.0) / 60.0 * width);
    filled = std::clamp(filled, 0, width);
    std::cout << "\033[90m[\033[m";
    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            double level = -60.0 + (static_cast<double>(i) + 0.5) * 60.0 / width;
            std::cout << dbfs_color(level) << "#\033[m";
        } else {
            std::cout << "\033[90m.\033[m";
        }
    }
    std::cout << "\033[90m]\033[m";
}

struct CaptureStats {
    double rms = 0.0;
    double peak = 0.0;
    uint64_t frames = 0;
};

CaptureStats measure_packet(const BYTE* data, UINT32 frames, const WAVEFORMATEX& fmt,
                            SampleEncoding encoding) {
    CaptureStats stats{};
    const UINT32 bps = fmt.wBitsPerSample / 8;
    double sumSq = 0.0;
    for (UINT32 f = 0; f < frames; ++f) {
        const BYTE* frameData = data + static_cast<size_t>(f) * fmt.nBlockAlign;
        for (WORD ch = 0; ch < fmt.nChannels; ++ch) {
            double val = decode_sample(frameData + ch * bps, encoding);
            double mag = std::abs(val);
            sumSq += val * val;
            stats.peak = std::max(stats.peak, mag);
        }
    }
    stats.rms = std::sqrt(sumSq / (static_cast<double>(frames) * fmt.nChannels));
    stats.frames = frames;
    return stats;
}

void encode_sample(BYTE* sample, SampleEncoding encoding, double value) {
    value = std::clamp(value, -1.0, 1.0);
    switch (encoding) {
    case SampleEncoding::Float32: {
        const float encoded = static_cast<float>(value);
        std::memcpy(sample, &encoded, sizeof(encoded));
        break;
    }
    case SampleEncoding::Pcm8: {
        const auto encoded = static_cast<BYTE>(std::llround((value + 1.0) * 127.5));
        *sample = encoded;
        break;
    }
    case SampleEncoding::Pcm16: {
        const auto encoded = static_cast<std::int16_t>(
            std::llround(value * (value < 0.0 ? 32768.0 : 32767.0)));
        std::memcpy(sample, &encoded, sizeof(encoded));
        break;
    }
    case SampleEncoding::Pcm24: {
        const auto encoded = static_cast<std::int32_t>(
            std::llround(value * (value < 0.0 ? 8388608.0 : 8388607.0)));
        sample[0] = static_cast<BYTE>(encoded & 0xFF);
        sample[1] = static_cast<BYTE>((encoded >> 8) & 0xFF);
        sample[2] = static_cast<BYTE>((encoded >> 16) & 0xFF);
        break;
    }
    case SampleEncoding::Pcm32: {
        const auto encoded = static_cast<std::int32_t>(
            std::llround(value * (value < 0.0 ? 2147483648.0 : 2147483647.0)));
        std::memcpy(sample, &encoded, sizeof(encoded));
        break;
    }
    }
}

std::vector<BYTE> apply_digital_gain(const BYTE* data, UINT32 frames,
                                     const WAVEFORMATEX& format,
                                     SampleEncoding encoding, double gain) {
    const size_t byteCount = static_cast<size_t>(frames) * format.nBlockAlign;
    std::vector<BYTE> processed(data, data + byteCount);
    const UINT32 bytesPerSample = format.wBitsPerSample / 8;
    for (UINT32 frame = 0; frame < frames; ++frame) {
        BYTE* frameData = processed.data() + static_cast<size_t>(frame) * format.nBlockAlign;
        for (WORD channel = 0; channel < format.nChannels; ++channel) {
            BYTE* sample = frameData + channel * bytesPerSample;
            encode_sample(sample, encoding, decode_sample(sample, encoding) * gain);
        }
    }
    return processed;
}

class AudioRingBuffer final {
public:
    AudioRingBuffer(UINT32 capacityFrames, UINT32 blockAlign, BYTE silenceValue)
        : data_(static_cast<size_t>(capacityFrames) * blockAlign),
          capacityFrames_(capacityFrames), blockAlign_(blockAlign),
          silenceValue_(silenceValue) {}

    void push(const BYTE* source, UINT32 frames) {
        if (frames >= capacityFrames_) {
            source += static_cast<size_t>(frames - capacityFrames_) * blockAlign_;
            frames = capacityFrames_;
            readFrame_ = 0;
            writeFrame_ = 0;
            storedFrames_ = 0;
        }
        const UINT32 required = frames > capacityFrames_ - storedFrames_
                                    ? frames - (capacityFrames_ - storedFrames_)
                                    : 0;
        readFrame_ = (readFrame_ + required) % capacityFrames_;
        storedFrames_ -= required;

        for (UINT32 frame = 0; frame < frames; ++frame) {
            std::memcpy(data_.data() + static_cast<size_t>(writeFrame_) * blockAlign_,
                        source + static_cast<size_t>(frame) * blockAlign_, blockAlign_);
            writeFrame_ = (writeFrame_ + 1) % capacityFrames_;
        }
        storedFrames_ += frames;
    }

    void push_silence(UINT32 frames) {
        std::vector<BYTE> silence(static_cast<size_t>(frames) * blockAlign_, silenceValue_);
        push(silence.data(), frames);
    }

    void fill_silence(BYTE* destination, UINT32 frames) const noexcept {
        std::memset(destination, silenceValue_, static_cast<size_t>(frames) * blockAlign_);
    }

    [[nodiscard]] UINT32 pop(BYTE* destination, UINT32 requestedFrames) {
        const UINT32 frames = std::min(requestedFrames, storedFrames_);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            std::memcpy(destination + static_cast<size_t>(frame) * blockAlign_,
                        data_.data() + static_cast<size_t>(readFrame_) * blockAlign_, blockAlign_);
            readFrame_ = (readFrame_ + 1) % capacityFrames_;
        }
        storedFrames_ -= frames;
        return frames;
    }

private:
    std::vector<BYTE> data_;
    UINT32 capacityFrames_;
    UINT32 blockAlign_;
    BYTE silenceValue_;
    UINT32 readFrame_ = 0;
    UINT32 writeFrame_ = 0;
    UINT32 storedFrames_ = 0;
};

void log_line(const CaptureStats& meter, const std::vector<SessionInfo>& sessions,
              double smoothPeakDb, double targetDb, double gainDb,
              std::string_view mode = "session volume") {
    std::cout << "\033[2J\033[H";
    std::cout << "LevelMate \xE2\x80\x93 real-time AGC via " << mode << "\n\n";

    std::cout << "  Raw:     ";
    print_meter_bar(to_dbfs(meter.peak), kMeterWidth);
    std::cout << " peak " << std::fixed << std::setprecision(1) << to_dbfs(meter.peak) << " dBFS"
              << "  rms " << to_dbfs(meter.rms) << " dBFS\n";

    std::cout << "  Smooth:  ";
    print_meter_bar(smoothPeakDb, kMeterWidth);
    std::cout << " " << std::setprecision(1) << smoothPeakDb << " dBFS\n";

    std::cout << "  Target:  ";
    print_meter_bar(targetDb, kMeterWidth);
    std::cout << " " << std::setprecision(1) << targetDb << " dBFS\n";

    std::cout << "  Gain:    "
              << (gainDb >= 0.0 ? "+" : "")
              << std::setprecision(1) << gainDb << " dB"
              << "  ("
              << std::setprecision(2) << from_dbfs(gainDb)
              << "x)\n";

    std::cout << "\n  Sessions (" << sessions.size() << "):\n";
    for (size_t i = 0; i < sessions.size() && i < 8; ++i) {
        float vol = 0.0f;
        const HRESULT volumeResult = sessions[i].volume->GetMasterVolume(&vol);
        std::cout << "  [" << i << "] PID " << sessions[i].processId;
        if (SUCCEEDED(volumeResult)) {
            std::cout << " vol=" << std::setprecision(2) << vol;
        } else {
            std::cout << " vol=unavailable";
        }
        std::cout << " (orig=" << std::setprecision(2) << sessions[i].originalVolume << ")";
        if (!sessions[i].displayName.empty()) {
            std::wcout << L" \"" << sessions[i].displayName << L"\"";
        }
        std::cout << "\n";
    }
    if (sessions.size() > 8) {
        std::cout << "  ... and " << (sessions.size() - 8) << " more\n";
    }

    std::cout << "\n  [Press Ctrl+C to stop]" << std::flush;
}

class AudioClientStopGuard final {
public:
    explicit AudioClientStopGuard(IAudioClient& client) : client_(client) {}
    ~AudioClientStopGuard() { client_.Stop(); }
    AudioClientStopGuard(const AudioClientStopGuard&) = delete;
    AudioClientStopGuard& operator=(const AudioClientStopGuard&) = delete;
private:
    IAudioClient& client_;
};

void run_rerender(const Options& options, std::vector<SessionInfo>& sessions,
                  IAudioClient& captureClient, const WAVEFORMATEX& format,
                  SampleEncoding encoding) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                  "CoCreateInstance(MMDeviceEnumerator)");
    ComPtr<IMMDevice> device;
    check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
                  "GetDefaultAudioEndpoint(render)");
    ComPtr<IAudioClient> renderClient;
    check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(renderClient.GetAddressOf())),
                  "IAudioClient::Activate(render)");

    check_hresult(renderClient->Initialize(
                      AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                      0, 0, &format, nullptr),
                  "IAudioClient::Initialize(render)");
    check_hresult(captureClient.Initialize(
                      AUDCLNT_SHAREMODE_SHARED,
                      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                      0, 0, &format, nullptr),
                  "IAudioClient::Initialize(capture)");

    UniqueHandle renderReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    UniqueHandle captureReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!renderReadyEvent || !captureReadyEvent) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent(rerender)");
    }
    check_hresult(renderClient->SetEventHandle(renderReadyEvent.get()),
                  "IAudioClient::SetEventHandle(render)");
    check_hresult(captureClient.SetEventHandle(captureReadyEvent.get()),
                  "IAudioClient::SetEventHandle(capture)");

    ComPtr<IAudioRenderClient> wasapiRender;
    check_hresult(renderClient->GetService(IID_PPV_ARGS(&wasapiRender)),
                  "IAudioClient::GetService(render)");
    ComPtr<IAudioCaptureClient> wasapiCapture;
    check_hresult(captureClient.GetService(IID_PPV_ARGS(&wasapiCapture)),
                  "IAudioClient::GetService(capture)");

    UINT32 renderBufferFrames = 0;
    check_hresult(renderClient->GetBufferSize(&renderBufferFrames),
                  "IAudioClient::GetBufferSize(render)");
    AudioRingBuffer queue(std::max<UINT32>(renderBufferFrames * 4, 1),
                          format.nBlockAlign,
                          encoding == SampleEncoding::Pcm8 ? 128 : 0);

    BYTE* initialBuffer = nullptr;
    check_hresult(wasapiRender->GetBuffer(renderBufferFrames, &initialBuffer),
                  "IAudioRenderClient::GetBuffer(initial)");
    check_hresult(wasapiRender->ReleaseBuffer(renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT),
                  "IAudioRenderClient::ReleaseBuffer(initial)");

    for (auto& session : sessions) {
        const float monitorVolume = static_cast<float>(
            static_cast<double>(session.originalVolume) * kRerenderMonitorScale);
        check_hresult(session.volume->SetMasterVolume(monitorVolume, nullptr),
                      "ISimpleAudioVolume::SetMasterVolume(rerender monitor)");
    }

    check_hresult(renderClient->Start(), "IAudioClient::Start(render)");
    AudioClientStopGuard renderStopGuard(*renderClient.Get());
    check_hresult(captureClient.Start(), "IAudioClient::Start(capture)");
    AudioClientStopGuard captureStopGuard(captureClient);

    std::cout << "Digital rerender enabled: the target's direct session is attenuated "
                 "and processed audio is being rendered to the default output.\n"
              << "Press Ctrl+C to stop.\n" << std::flush;

    double smoothDb = kSilenceGateDbfs;
    double currentGainDb = 0.0;
    CaptureStats accumulatedStats{};
    const auto startedAt = std::chrono::steady_clock::now();
    auto nextReport = startedAt + kReportInterval;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (options.duration && now - startedAt >= *options.duration) {
            break;
        }
        const auto untilReport = std::max(std::chrono::milliseconds::zero(),
            std::chrono::duration_cast<std::chrono::milliseconds>(nextReport - now));
        const DWORD waitMs = static_cast<DWORD>(
            std::min<std::int64_t>(untilReport.count(), std::numeric_limits<DWORD>::max()));
        const HANDLE events[] = {captureReadyEvent.get(), renderReadyEvent.get(), g_stopEvent};
        const DWORD waitResult = WaitForMultipleObjects(3, events, FALSE, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            UINT32 packetFrames = 0;
            check_hresult(wasapiCapture->GetNextPacketSize(&packetFrames), "GetNextPacketSize");
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                check_hresult(wasapiCapture->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                              "IAudioCaptureClient::GetBuffer");

                CaptureStats stats{};
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                    stats.frames = frames;
                    queue.push_silence(frames);
                    currentGainDb = 0.0;
                } else {
                    if (data == nullptr) {
                        throw std::runtime_error("WASAPI returned a null non-silent buffer");
                    }
                    stats = measure_packet(data, frames, format, encoding);
                    stats.peak = std::clamp(
                        stats.peak / kRerenderMonitorScale, 0.0, 1.0);
                    stats.rms = std::clamp(
                        stats.rms / kRerenderMonitorScale, 0.0, 1.0);
                    const double peakDb = to_dbfs(stats.peak);
                    double gain = 1.0;
                    if (std::isfinite(peakDb) && peakDb >= kSilenceGateDbfs) {
                        const double packetMs = static_cast<double>(frames) * 1000.0 /
                                                format.nSamplesPerSec;
                        gain = calculate_smoothed_gain(peakDb, smoothDb, packetMs);
                        // The smoother controls perceived loudness; this packet ceiling
                        // catches spikes immediately and prevents digital clipping.
                        gain = apply_peak_limiter(gain, stats.peak);
                        currentGainDb = to_dbfs(gain);
                    } else {
                        currentGainDb = 0.0;
                    }
                    const auto processed = apply_digital_gain(
                        data, frames, format, encoding,
                        gain / kRerenderMonitorScale);
                    queue.push(processed.data(), frames);
                }

                accumulatedStats.peak = std::max(accumulatedStats.peak, stats.peak);
                const uint64_t combinedFrames = accumulatedStats.frames + stats.frames;
                if (combinedFrames > 0) {
                    accumulatedStats.rms = std::sqrt(
                        (accumulatedStats.rms * accumulatedStats.rms * accumulatedStats.frames +
                         stats.rms * stats.rms * stats.frames) /
                        static_cast<double>(combinedFrames));
                }
                accumulatedStats.frames = combinedFrames;

                check_hresult(wasapiCapture->ReleaseBuffer(frames),
                              "IAudioCaptureClient::ReleaseBuffer");
                check_hresult(wasapiCapture->GetNextPacketSize(&packetFrames),
                              "GetNextPacketSize");
            }
        } else if (waitResult == WAIT_OBJECT_0 + 1) {
            UINT32 padding = 0;
            check_hresult(renderClient->GetCurrentPadding(&padding),
                          "IAudioClient::GetCurrentPadding(render)");
            const UINT32 availableFrames = renderBufferFrames - padding;
            if (availableFrames > 0) {
                BYTE* destination = nullptr;
                check_hresult(wasapiRender->GetBuffer(availableFrames, &destination),
                              "IAudioRenderClient::GetBuffer");
                const UINT32 copiedFrames = queue.pop(destination, availableFrames);
                const UINT32 missingFrames = availableFrames - copiedFrames;
                if (missingFrames > 0) {
                    queue.fill_silence(
                        destination + static_cast<size_t>(copiedFrames) * format.nBlockAlign,
                        missingFrames);
                }
                check_hresult(wasapiRender->ReleaseBuffer(
                                  availableFrames,
                                  copiedFrames == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0),
                              "IAudioRenderClient::ReleaseBuffer");
            }
        } else if (waitResult == WAIT_OBJECT_0 + 2) {
            break;
        } else if (waitResult != WAIT_TIMEOUT) {
            check_hresult(HRESULT_FROM_WIN32(GetLastError()), "WaitForMultipleObjects(rerender)");
        }

        const auto afterAction = std::chrono::steady_clock::now();
        if (afterAction >= nextReport) {
            if (accumulatedStats.frames > 0) {
                log_line(accumulatedStats, sessions, smoothDb, kTargetDbfs,
                         currentGainDb, "digital rerender");
            }
            accumulatedStats = {};
            nextReport = afterAction + kReportInterval;
        }
    }
}

void run_probe(const Options& options) {
    validate_process(options.processId);

    std::vector<DWORD> treePids = get_process_tree(options.processId);
    if (treePids.empty()) {
        treePids.push_back(options.processId);
    }

    auto sessions = enumerate_sessions(treePids);
    if (sessions.empty()) {
        std::cout << "No active audio sessions found for the process tree (PID "
                  << options.processId << ").\n"
                  << "Start audio in the target application and re-run.\n"
                  << std::flush;
        return;
    }
    SessionRestoreGuard sessionRestoreGuard(sessions);

    std::cout << "Found " << sessions.size() << " audio session(s) for "
              << treePids.size() << " target processes.\n";

    ComPtr<IAudioClient> captureClient = activate_process_loopback(options.processId);

    CoTaskMemWaveFormat captureFormat;
    HRESULT hr = captureClient->GetMixFormat(captureFormat.put());
    if (hr == E_NOTIMPL) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                      "CoCreateInstance(MMDeviceEnumerator)");
        ComPtr<IMMDevice> device;
        check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
                      "GetDefaultAudioEndpoint");
        ComPtr<IAudioClient> defaultClient;
        check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                       nullptr, reinterpret_cast<void**>(defaultClient.GetAddressOf())),
                      "IAudioClient::Activate(default)");
        check_hresult(defaultClient->GetMixFormat(captureFormat.put()),
                      "IAudioClient::GetMixFormat(fallback)");
    } else {
        check_hresult(hr, "IAudioClient::GetMixFormat(capture)");
    }
    const SampleEncoding encoding = sample_encoding(*captureFormat.get());

    if (options.rerender) {
        run_rerender(options, sessions, *captureClient.Get(), *captureFormat.get(), encoding);
        std::cout << "\n\nRestoring original volumes...\n" << std::flush;
        if (!restore_sessions(sessions)) {
            throw std::runtime_error("One or more target session volumes could not be restored");
        }
        std::cout << "Done.\n" << std::flush;
        return;
    }

    check_hresult(captureClient->Initialize(
                      AUDCLNT_SHAREMODE_SHARED,
                      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                      0, 0, captureFormat.get(), nullptr),
                  "IAudioClient::Initialize(capture)");

    UniqueHandle captureReadyEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!captureReadyEvent) {
        check_hresult(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent(capture-ready)");
    }
    check_hresult(captureClient->SetEventHandle(captureReadyEvent.get()),
                  "IAudioClient::SetEventHandle(capture)");

    ComPtr<IAudioCaptureClient> wasapiCapture;
    check_hresult(captureClient->GetService(IID_PPV_ARGS(&wasapiCapture)),
                  "IAudioClient::GetService(capture)");

    check_hresult(captureClient->Start(), "IAudioClient::Start(capture)");
    AudioClientStopGuard captureStopGuard(*captureClient.Get());

    double smoothDb = -60.0;
    double currentGainDb = 0.0;

    std::cout << "Target: " << static_cast<int>(kTargetDbfs) << " dBFS"
              << " | attack: " << static_cast<int>(kAttackMs) << " ms"
              << " | release: " << static_cast<int>(kReleaseMs) << " ms\n"
              << "Press Ctrl+C to stop.\n" << std::flush;

    const auto startedAt = std::chrono::steady_clock::now();
    auto nextAction = startedAt;
    CaptureStats accumulatedStats{};

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (options.duration && now - startedAt >= *options.duration) break;

        const auto untilAction = std::max(std::chrono::milliseconds::zero(),
            std::chrono::duration_cast<std::chrono::milliseconds>(nextAction - now));
        const DWORD waitMs = static_cast<DWORD>(
            std::min<std::int64_t>(untilAction.count(), std::numeric_limits<DWORD>::max()));
        const HANDLE events[] = {captureReadyEvent.get(), g_stopEvent};
        const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            UINT32 packetFrames = 0;
            check_hresult(wasapiCapture->GetNextPacketSize(&packetFrames), "GetNextPacketSize");
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT32 frames = 0;
                check_hresult(wasapiCapture->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                              "IAudioCaptureClient::GetBuffer");

                CaptureStats stats{};
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                    stats.frames = frames;
                } else {
                    if (data == nullptr) {
                        throw std::runtime_error("WASAPI returned a null non-silent buffer");
                    }
                    stats = measure_packet(data, frames, *captureFormat.get(), encoding);
                }
                if (stats.peak > accumulatedStats.peak)
                    accumulatedStats.peak = stats.peak;
                const uint64_t combinedFrames = accumulatedStats.frames + stats.frames;
                if (combinedFrames > 0) {
                    accumulatedStats.rms = std::sqrt(
                        (accumulatedStats.rms * accumulatedStats.rms * accumulatedStats.frames +
                         stats.rms * stats.rms * stats.frames) /
                        static_cast<double>(combinedFrames));
                }
                accumulatedStats.frames += stats.frames;

                check_hresult(wasapiCapture->ReleaseBuffer(frames),
                              "IAudioCaptureClient::ReleaseBuffer");
                check_hresult(wasapiCapture->GetNextPacketSize(&packetFrames),
                              "GetNextPacketSize");
            }
        } else if (waitResult == WAIT_OBJECT_0 + 1) {
            break;
        } else if (waitResult != WAIT_TIMEOUT) {
            check_hresult(HRESULT_FROM_WIN32(GetLastError()), "WaitForMultipleObjects");
        }

        const auto afterAction = std::chrono::steady_clock::now();
        if (afterAction >= nextAction) {
            if (accumulatedStats.frames > 0) {
                const double peakDb = to_dbfs(accumulatedStats.peak);
                const double desiredGainLin = calculate_smoothed_gain(
                    peakDb, smoothDb, static_cast<double>(kReportInterval.count()));
                if (std::isfinite(peakDb) && peakDb >= kSilenceGateDbfs) {
                    currentGainDb = to_dbfs(desiredGainLin);
                } else {
                    // Never turn digital silence or sub-gate noise into maximum gain.
                    // Returning to unity also avoids a loud transient after a pause.
                    currentGainDb = 0.0;
                }

                for (auto& s : sessions) {
                    const float newVol = static_cast<float>(
                        std::clamp(static_cast<double>(s.originalVolume) * desiredGainLin, 0.0, 1.0));
                    check_hresult(s.volume->SetMasterVolume(newVol, nullptr),
                                  "ISimpleAudioVolume::SetMasterVolume(AGC)");
                }

                log_line(accumulatedStats, sessions, smoothDb, kTargetDbfs, currentGainDb);
            }

            accumulatedStats = {};
            nextAction = afterAction + kReportInterval;
        }
    }

    std::cout << "\n\nRestoring original volumes...\n" << std::flush;
    if (!restore_sessions(sessions)) {
        throw std::runtime_error("One or more target session volumes could not be restored");
    }
    std::cout << "Done.\n" << std::flush;
}

}  // namespace

#ifndef LEVELMATE_TESTING
int wmain(int argc, wchar_t* argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_usage();
        return 2;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex
                   << static_cast<unsigned long>(comResult) << L" "
                   << format_hresult(comResult) << L'\n';
        return 1;
    }

    UniqueHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        std::wcerr << L"CreateEvent(stop) failed: 0x" << std::hex
                   << static_cast<unsigned long>(result) << L" "
                   << format_hresult(result) << L'\n';
        CoUninitialize();
        return 1;
    }
    g_stopEvent = stopEvent.get();
    SetConsoleCtrlHandler(console_control_handler, TRUE);

    int exitCode = 0;
    try {
        run_probe(*options);
    } catch (const HrError& error) {
        std::wcerr << std::wstring(error.what(), error.what() + std::strlen(error.what()))
                   << L" failed: 0x" << std::hex
                   << static_cast<unsigned long>(error.result()) << L" "
                   << format_hresult(error.result()) << L'\n';
        exitCode = 1;
    } catch (const std::exception& error) {
        std::cerr << "Probe failed: " << error.what() << '\n';
        exitCode = 1;
    }

    SetConsoleCtrlHandler(console_control_handler, FALSE);
    g_stopEvent = nullptr;
    CoUninitialize();
    return exitCode;
}
#endif
