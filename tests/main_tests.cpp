#define LEVELMATE_TESTING
#include "../src/main.cpp"

#include <array>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_near(double actual, double expected, double tolerance,
                 std::string_view message) {
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        std::abs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " (expected " << expected
                  << ", got " << actual << ")\n";
        ++failures;
    }
}

template <typename Callable>
void expect_throws(Callable&& callable, std::string_view message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::optional<Options> parse(std::initializer_list<std::wstring_view> arguments) {
    std::vector<std::wstring> storage;
    storage.reserve(arguments.size());
    for (const auto argument : arguments) {
        storage.emplace_back(argument);
    }
    std::vector<wchar_t*> pointers;
    pointers.reserve(storage.size());
    for (auto& argument : storage) {
        pointers.push_back(argument.data());
    }
    return parse_options(static_cast<int>(pointers.size()), pointers.data());
}

void test_options() {
    const auto basic = parse({L"levelmate.exe", L"1234"});
    expect(basic && basic->processId == 1234, "accepts a generic target PID");
    expect(basic && !basic->duration && !basic->rerender,
           "uses safe default options");

    const auto full = parse(
        {L"levelmate.exe", L"42", L"--rerender", L"--duration", L"30"});
    expect(full && full->processId == 42 && full->rerender,
           "accepts rerender mode");
    expect(full && full->duration == std::chrono::seconds(30),
           "accepts a fixed duration");

    const auto reordered = parse(
        {L"levelmate.exe", L"42", L"--duration", L"30", L"--rerender"});
    expect(reordered && reordered->rerender,
           "accepts flags in either supported order");

    expect(!parse({L"levelmate.exe", L"0"}), "rejects PID zero");
    expect(!parse({L"levelmate.exe", L"abc"}), "rejects a non-numeric PID");
    expect(!parse({L"levelmate.exe", L"42", L"--duration"}),
           "rejects a missing duration value");
    expect(!parse({L"levelmate.exe", L"42", L"--duration", L"0"}),
           "rejects a zero duration");
    expect(!parse({L"levelmate.exe", L"42", L"--duration", L"-1"}),
           "rejects a negative duration");
    expect(!parse({L"levelmate.exe", L"42", L"--duration", L"30", L"--duration", L"60"}),
           "rejects duplicate duration flags");
    expect(!parse({L"levelmate.exe", L"42", L"--rerender", L"--rerender"}),
           "rejects duplicate rerender flags");
    expect(!parse({L"levelmate.exe", L"42", L"--unknown"}),
           "rejects unknown flags");
    expect(!parse({L"levelmate.exe"}), "rejects a missing PID");
    expect(!parse({L"levelmate.exe", L"-1"}), "rejects a negative PID");
    expect(!parse({L"levelmate.exe", L"4294967296"}),
           "rejects a PID above DWORD range");
    expect(!parse({L"levelmate.exe", L"42", L"--duration", L"9223372036854775808"}),
           "rejects a duration above signed 64-bit range");
}

void test_gain_control() {
    double smooth = -20.0;
    const double silentGain = calculate_smoothed_gain(
        -std::numeric_limits<double>::infinity(), smooth, 250.0);
    expect_near(silentGain, 1.0, 1e-12, "digital silence uses unity gain");
    expect_near(smooth, -20.0, 1e-12, "digital silence does not poison smoothing");

    const double gatedGain = calculate_smoothed_gain(-61.0, smooth, 250.0);
    expect_near(gatedGain, 1.0, 1e-12, "sub-gate noise is not amplified");
    expect_near(smooth, -20.0, 1e-12, "sub-gate noise leaves smoothing unchanged");

    smooth = -40.0;
    const double quietGain = calculate_smoothed_gain(-40.0, smooth, 250.0);
    expect_near(quietGain, from_dbfs(kMaxGainDb), 1e-9,
                "quiet content respects the maximum gain");

    smooth = -60.0;
    const double loudGain = calculate_smoothed_gain(0.0, smooth, 1000.0);
    expect(loudGain < 1.0, "loud content is attenuated");
    expect(loudGain >= from_dbfs(kMinGainDb), "attenuation respects the minimum gain");

    expect_near(apply_peak_limiter(10.0, 1.0), from_dbfs(kTargetDbfs), 1e-12,
                "instantaneous limiter catches a full-scale spike");
    expect_near(apply_peak_limiter(2.0, 0.01), 2.0, 1e-12,
                "instantaneous limiter preserves safe gain");
}

void test_sample_processing() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    std::array<float, 2> input{0.25F, -0.75F};
    const auto doubled = apply_digital_gain(
        reinterpret_cast<const BYTE*>(input.data()), 1, format,
        SampleEncoding::Float32, 2.0);
    expect_near(decode_sample(doubled.data(), SampleEncoding::Float32), 0.5, 1e-6,
                "digital gain scales float samples");
    expect_near(decode_sample(doubled.data() + sizeof(float), SampleEncoding::Float32),
                -1.0, 1e-6, "digital gain clips negative float samples safely");

    for (const auto encoding : {SampleEncoding::Float32, SampleEncoding::Pcm8,
                                SampleEncoding::Pcm16, SampleEncoding::Pcm24,
                                SampleEncoding::Pcm32}) {
        std::array<BYTE, 4> bytes{};
        encode_sample(bytes.data(), encoding, 0.5);
        const double tolerance = encoding == SampleEncoding::Pcm8 ? 0.01 : 0.0001;
        expect_near(decode_sample(bytes.data(), encoding), 0.5, tolerance,
                    "sample encoding round-trips positive values");
        encode_sample(bytes.data(), encoding, -0.5);
        expect_near(decode_sample(bytes.data(), encoding), -0.5, tolerance,
                    "sample encoding round-trips negative values");
    }

    std::array<BYTE, 4> pcm8{};
    encode_sample(pcm8.data(), SampleEncoding::Pcm8, 0.0);
    expect(pcm8[0] == 128, "PCM8 encodes zero at the unsigned midpoint");
    encode_sample(pcm8.data(), SampleEncoding::Pcm8, 1.0);
    expect(pcm8[0] == 255, "PCM8 encodes full-scale positive samples");
    encode_sample(pcm8.data(), SampleEncoding::Pcm8, -1.0);
    expect(pcm8[0] == 0, "PCM8 encodes full-scale negative samples");

    std::array<BYTE, 4> pcm16{};
    encode_sample(pcm16.data(), SampleEncoding::Pcm16, 0.5);
    expect(pcm16[0] == 0x00 && pcm16[1] == 0x40,
           "PCM16 encodes little-endian positive samples");
    encode_sample(pcm16.data(), SampleEncoding::Pcm16, -1.0);
    expect(pcm16[0] == 0x00 && pcm16[1] == 0x80,
           "PCM16 encodes little-endian negative full scale");

    std::array<BYTE, 4> pcm24{};
    encode_sample(pcm24.data(), SampleEncoding::Pcm24, 0.5);
    expect(pcm24[0] == 0x00 && pcm24[1] == 0x00 && pcm24[2] == 0x40,
           "PCM24 encodes little-endian positive samples");
    encode_sample(pcm24.data(), SampleEncoding::Pcm24, -1.0);
    expect(pcm24[0] == 0x00 && pcm24[1] == 0x00 && pcm24[2] == 0x80,
           "PCM24 encodes little-endian negative full scale");

    std::array<BYTE, 4> pcm32{};
    encode_sample(pcm32.data(), SampleEncoding::Pcm32, 0.5);
    expect(pcm32[0] == 0x00 && pcm32[1] == 0x00 &&
               pcm32[2] == 0x00 && pcm32[3] == 0x40,
           "PCM32 encodes little-endian positive samples");
    encode_sample(pcm32.data(), SampleEncoding::Pcm32, -1.0);
    expect(pcm32[0] == 0x00 && pcm32[1] == 0x00 &&
               pcm32[2] == 0x00 && pcm32[3] == 0x80,
           "PCM32 encodes little-endian negative full scale");

    WAVEFORMATEX pcm16Format{};
    pcm16Format.wFormatTag = WAVE_FORMAT_PCM;
    pcm16Format.nChannels = 2;
    pcm16Format.nSamplesPerSec = 48000;
    pcm16Format.wBitsPerSample = 16;
    pcm16Format.nBlockAlign = 4;
    pcm16Format.nAvgBytesPerSec = pcm16Format.nSamplesPerSec * pcm16Format.nBlockAlign;
    std::array<BYTE, 4> pcm16Frame{};
    encode_sample(pcm16Frame.data(), SampleEncoding::Pcm16, 0.25);
    encode_sample(pcm16Frame.data() + 2, SampleEncoding::Pcm16, -0.25);
    const auto pcm16Doubled = apply_digital_gain(
        pcm16Frame.data(), 1, pcm16Format, SampleEncoding::Pcm16, 2.0);
    expect_near(decode_sample(pcm16Doubled.data(), SampleEncoding::Pcm16),
                0.5, 0.0001, "digital gain scales PCM16 samples");
    expect_near(decode_sample(pcm16Doubled.data() + 2, SampleEncoding::Pcm16),
                -0.5, 0.0001, "digital gain scales negative PCM16 samples");
}

void test_sample_formats() {
    WAVEFORMATEX floatFormat{};
    floatFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    floatFormat.wBitsPerSample = 32;
    expect(sample_encoding(floatFormat) == SampleEncoding::Float32,
           "recognizes 32-bit float formats");

    WAVEFORMATEX pcm24Format{};
    pcm24Format.wFormatTag = WAVE_FORMAT_PCM;
    pcm24Format.wBitsPerSample = 24;
    expect(sample_encoding(pcm24Format) == SampleEncoding::Pcm24,
           "recognizes 24-bit PCM formats");

    WAVEFORMATEXTENSIBLE extensibleFloat{};
    extensibleFloat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensibleFloat.Format.wBitsPerSample = 32;
    extensibleFloat.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensibleFloat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    expect(sample_encoding(extensibleFloat.Format) == SampleEncoding::Float32,
           "recognizes extensible float formats");

    WAVEFORMATEXTENSIBLE extensiblePcm{};
    extensiblePcm.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensiblePcm.Format.wBitsPerSample = 16;
    extensiblePcm.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensiblePcm.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    expect(sample_encoding(extensiblePcm.Format) == SampleEncoding::Pcm16,
           "recognizes extensible PCM formats");

    WAVEFORMATEX unsupportedBits{};
    unsupportedBits.wFormatTag = WAVE_FORMAT_PCM;
    unsupportedBits.wBitsPerSample = 20;
    expect_throws([&] { sample_encoding(unsupportedBits); },
                  "rejects unsupported PCM bit depths");

    WAVEFORMATEXTENSIBLE shortExtensible{};
    shortExtensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    shortExtensible.Format.cbSize = 0;
    expect_throws([&] { sample_encoding(shortExtensible.Format); },
                  "rejects undersized extensible formats");

    WAVEFORMATEXTENSIBLE unsupportedSubformat{};
    unsupportedSubformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    unsupportedSubformat.Format.wBitsPerSample = 32;
    unsupportedSubformat.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    unsupportedSubformat.SubFormat = GUID{};
    expect_throws([&] { sample_encoding(unsupportedSubformat.Format); },
                  "rejects unsupported extensible subformats");
}

void test_packet_measurement() {
    WAVEFORMATEX floatFormat{};
    floatFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    floatFormat.nChannels = 2;
    floatFormat.nSamplesPerSec = 48000;
    floatFormat.wBitsPerSample = 32;
    floatFormat.nBlockAlign = 8;
    floatFormat.nAvgBytesPerSec = floatFormat.nSamplesPerSec * floatFormat.nBlockAlign;

    const std::array<float, 4> floatSamples{0.5F, -0.5F, 0.25F, -0.25F};
    const auto floatStats = measure_packet(
        reinterpret_cast<const BYTE*>(floatSamples.data()), 2,
        floatFormat, SampleEncoding::Float32);
    expect(floatStats.frames == 2, "packet measurement reports frame count");
    expect_near(floatStats.peak, 0.5, 1e-12, "packet measurement reports peak");
    expect_near(floatStats.rms, std::sqrt(0.15625), 1e-12,
                "packet measurement reports multi-channel RMS");

    WAVEFORMATEX pcm16Format{};
    pcm16Format.wFormatTag = WAVE_FORMAT_PCM;
    pcm16Format.nChannels = 1;
    pcm16Format.nSamplesPerSec = 48000;
    pcm16Format.wBitsPerSample = 16;
    pcm16Format.nBlockAlign = 2;
    pcm16Format.nAvgBytesPerSec = pcm16Format.nSamplesPerSec * pcm16Format.nBlockAlign;
    std::array<BYTE, 4> pcmSamples{};
    encode_sample(pcmSamples.data(), SampleEncoding::Pcm16, 0.25);
    encode_sample(pcmSamples.data() + 2, SampleEncoding::Pcm16, -0.75);
    const auto pcmStats = measure_packet(
        pcmSamples.data(), 2, pcm16Format, SampleEncoding::Pcm16);
    expect_near(pcmStats.peak, 0.75, 0.0001,
                "packet measurement decodes PCM peaks");
    expect_near(pcmStats.rms, std::sqrt((0.25 * 0.25 + 0.75 * 0.75) / 2.0),
                0.0001, "packet measurement decodes PCM RMS");
}

void test_ring_buffer() {
    AudioRingBuffer queue(4, 1, 0);
    const std::array<BYTE, 3> first{1, 2, 3};
    queue.push(first.data(), static_cast<UINT32>(first.size()));

    std::array<BYTE, 4> output{};
    expect(queue.pop(output.data(), 2) == 2, "ring buffer returns available frames");
    expect(output[0] == 1 && output[1] == 2, "ring buffer preserves FIFO order");

    const std::array<BYTE, 3> second{4, 5, 6};
    queue.push(second.data(), static_cast<UINT32>(second.size()));
    expect(queue.pop(output.data(), 4) == 4, "ring buffer wraps at capacity");
    expect(output == std::array<BYTE, 4>{3, 4, 5, 6},
           "ring buffer preserves wrapped data order");

    const std::array<BYTE, 5> overflow{1, 2, 3, 4, 5};
    queue.push(overflow.data(), static_cast<UINT32>(overflow.size()));
    expect(queue.pop(output.data(), 4) == 4, "ring buffer caps oversized writes");
    expect(output == std::array<BYTE, 4>{2, 3, 4, 5},
           "ring buffer keeps the newest frames on overflow");

    AudioRingBuffer pcm8Queue(2, 1, 128);
    pcm8Queue.push_silence(2);
    expect(pcm8Queue.pop(output.data(), 2) == 2 && output[0] == 128 && output[1] == 128,
           "PCM8 silence uses its midpoint representation");

    expect(pcm8Queue.pop(output.data(), 2) == 0,
           "ring buffer reports empty pops");

    AudioRingBuffer stereoQueue(3, 2, 0);
    const std::array<BYTE, 6> stereoFirst{1, 10, 2, 20, 3, 30};
    stereoQueue.push(stereoFirst.data(), 3);
    std::array<BYTE, 6> stereoOutput{};
    expect(stereoQueue.pop(stereoOutput.data(), 1) == 1,
           "ring buffer pops one multi-byte frame");
    expect(stereoOutput[0] == 1 && stereoOutput[1] == 10,
           "ring buffer preserves multi-byte frame contents");
    const std::array<BYTE, 4> stereoSecond{4, 40, 5, 50};
    stereoQueue.push(stereoSecond.data(), 2);
    expect(stereoQueue.pop(stereoOutput.data(), 3) == 3,
           "ring buffer wraps multi-byte frames");
    expect(stereoOutput == std::array<BYTE, 6>{3, 30, 4, 40, 5, 50},
           "ring buffer drops oldest multi-byte frames on overflow");

    AudioRingBuffer stereoSilenceQueue(2, 2, 0x7F);
    stereoSilenceQueue.push_silence(3);
    expect(stereoSilenceQueue.pop(stereoOutput.data(), 2) == 2 &&
               stereoOutput[0] == 0x7F && stereoOutput[1] == 0x7F &&
               stereoOutput[2] == 0x7F && stereoOutput[3] == 0x7F,
           "ring buffer silence overflow fills complete multi-byte frames");
}

void test_recovery_serialization() {
    const std::vector<RecoveryEntry> entries{
        {0.75F, 0.25F, L"session-one", L"instance-one"},
        {1.0F, 0.5F, L"session-two", L"instance-two"},
    };
    const auto encoded = serialize_recovery_entries(entries);
    const auto decoded = deserialize_recovery_entries(encoded);
    expect(decoded && decoded->size() == entries.size(),
           "recovery journal round-trips its entries");
    expect(decoded && (*decoded)[0].sessionIdentifier == L"session-one" &&
               (*decoded)[0].instanceIdentifier == L"instance-one",
           "recovery journal preserves session identities");
    expect(decoded && std::abs((*decoded)[1].originalVolume - 1.0F) < 0.0001F &&
               std::abs((*decoded)[1].appliedVolume - 0.5F) < 0.0001F,
           "recovery journal preserves volume values");

    auto truncated = encoded;
    truncated.pop_back();
    expect(!deserialize_recovery_entries(truncated),
           "recovery journal rejects truncated data");

    auto invalidMagic = encoded;
    invalidMagic[0] ^= 0xFF;
    expect(!deserialize_recovery_entries(invalidMagic),
           "recovery journal rejects an invalid header");

    auto invalidVersion = encoded;
    const std::uint32_t unsupportedVersion = kRecoveryVersion + 1;
    std::memcpy(invalidVersion.data() + sizeof(std::uint32_t),
                &unsupportedVersion, sizeof(unsupportedVersion));
    expect(!deserialize_recovery_entries(invalidVersion),
           "recovery journal rejects unsupported versions");

    auto withTrailingBytes = encoded;
    withTrailingBytes.push_back(0);
    expect(!deserialize_recovery_entries(withTrailingBytes),
           "recovery journal rejects trailing bytes");

    auto invalidCount = encoded;
    const std::uint32_t excessiveCount = 4097;
    std::memcpy(invalidCount.data() + sizeof(std::uint32_t) * 2,
                &excessiveCount, sizeof(excessiveCount));
    expect(!deserialize_recovery_entries(invalidCount),
           "recovery journal rejects excessive entry counts");

    auto invalidVolume = encoded;
    const float outOfRangeVolume = 1.5F;
    std::memcpy(invalidVolume.data() + sizeof(std::uint32_t) * 3,
                &outOfRangeVolume, sizeof(outOfRangeVolume));
    expect(!deserialize_recovery_entries(invalidVolume),
           "recovery journal rejects out-of-range volumes");

    auto nanVolume = encoded;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nanVolume.data() + sizeof(std::uint32_t) * 3,
                &nan, sizeof(nan));
    expect(!deserialize_recovery_entries(nanVolume),
           "recovery journal rejects NaN volumes");

    auto infiniteVolume = encoded;
    const float infinity = std::numeric_limits<float>::infinity();
    std::memcpy(infiniteVolume.data() + sizeof(std::uint32_t) * 3 + sizeof(float),
                &infinity, sizeof(infinity));
    expect(!deserialize_recovery_entries(infiniteVolume),
           "recovery journal rejects infinite volumes");

    auto invalidStringLength = encoded;
    const std::uint32_t impossibleLength = 0xFFFFFFFF;
    constexpr size_t firstStringLengthOffset =
        sizeof(std::uint32_t) * 3 + sizeof(float) * 2;
    std::memcpy(invalidStringLength.data() + firstStringLengthOffset,
                &impossibleLength, sizeof(impossibleLength));
    expect(!deserialize_recovery_entries(invalidStringLength),
           "recovery journal rejects malformed string lengths");
    expect(!deserialize_recovery_entries({}),
           "recovery journal rejects empty byte streams");

    expect(is_missing_recovery_file_error(ERROR_FILE_NOT_FOUND),
           "recovery read treats a missing file as empty");
    expect(is_missing_recovery_file_error(ERROR_PATH_NOT_FOUND),
           "recovery read treats a missing directory as empty");
    expect(!is_missing_recovery_file_error(ERROR_ACCESS_DENIED),
           "recovery read still reports non-missing errors");
}

void test_recovery_policy() {
    RecoveryEntry pending{0.75F, 0.25F, L"session-one", L"instance-one"};

    SessionInfo instanceMatch{};
    instanceMatch.sessionIdentifier = L"different-session";
    instanceMatch.instanceIdentifier = L"instance-one";
    std::vector<SessionInfo> sessions{instanceMatch};
    expect(find_recovery_session(sessions, pending) == sessions.begin(),
           "recovery matches the stable session instance identifier first");

    SessionInfo sessionMatch{};
    sessionMatch.sessionIdentifier = L"session-one";
    sessionMatch.instanceIdentifier = L"";
    sessions = {sessionMatch};
    expect(find_recovery_session(sessions, pending) == sessions.begin(),
           "recovery falls back to the session identifier");

    SessionInfo unrelated{};
    unrelated.sessionIdentifier = L"session-two";
    unrelated.instanceIdentifier = L"instance-two";
    sessions = {unrelated};
    expect(find_recovery_session(sessions, pending) == sessions.end(),
           "unmatched recovery entries remain unresolved for a future launch");

    expect(!recovery_volume_was_changed_manually(0.25005F, pending),
           "recovery tolerates small volume rounding differences");
    expect(recovery_volume_was_changed_manually(0.5F, pending),
           "recovery preserves newer manual volume changes");
}

}  // namespace

int main() {
    test_options();
    test_gain_control();
    test_sample_processing();
    test_sample_formats();
    test_packet_measurement();
    test_ring_buffer();
    test_recovery_serialization();
    test_recovery_policy();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All LevelMate tests passed.\n";
    return 0;
}
