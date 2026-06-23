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
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " (expected " << expected
                  << ", got " << actual << ")\n";
        ++failures;
    }
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
    expect(!parse({L"levelmate.exe", L"42", L"--rerender", L"--rerender"}),
           "rejects duplicate rerender flags");
    expect(!parse({L"levelmate.exe", L"42", L"--unknown"}),
           "rejects unknown flags");
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

    expect(is_missing_recovery_file_error(ERROR_FILE_NOT_FOUND),
           "recovery read treats a missing file as empty");
    expect(is_missing_recovery_file_error(ERROR_PATH_NOT_FOUND),
           "recovery read treats a missing directory as empty");
    expect(!is_missing_recovery_file_error(ERROR_ACCESS_DENIED),
           "recovery read still reports non-missing errors");
}

}  // namespace

int main() {
    test_options();
    test_gain_control();
    test_sample_processing();
    test_ring_buffer();
    test_recovery_serialization();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All LevelMate tests passed.\n";
    return 0;
}
