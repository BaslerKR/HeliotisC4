#include "HeliotisC4System.h"
#include "Internal/HeliotisAcquisitionPolicy.h"
#include "Internal/C4UtilitySdkRuntime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace heliotis {

namespace {

std::string lastSdkError()
{
    const char* error = C4Hdl_getLastError();
    return error ? std::string(error) : std::string("Unknown C4Utility error.");
}

std::string operationError(const char* operation, const C4HDL_ERROR result)
{
    return std::string(operation) + " returned C4Utility code "
        + std::to_string(static_cast<long long>(result)) + ": " + lastSdkError();
}

void logInfo(const std::string& message);

bool checkBufferOperation(
    const C4HDL_ERROR result,
    const char* operation,
    std::string* errorMessage)
{
    if (result == C4HDL_ERR_SUCCESS) return true;
    if (errorMessage) *errorMessage = operationError(operation, result);
    return false;
}

template <typename Sample, typename Reader>
bool copyBufferPartSamples(
    const std::uint32_t expectedSamples,
    const char* operation,
    Reader&& reader,
    std::vector<Sample>* samples,
    std::string* errorMessage)
{
    if (expectedSamples > (std::numeric_limits<std::uint32_t>::max)() / sizeof(Sample)) {
        if (errorMessage) *errorMessage = std::string(operation) + " exceeds C4Utility's 32-bit byte-capacity limit.";
        return false;
    }
    std::uint32_t capacity = expectedSamples * static_cast<std::uint32_t>(sizeof(Sample));
    for (int attempt = 0; attempt != 2; ++attempt) {
        samples->resize((capacity + sizeof(Sample) - 1) / sizeof(Sample));
        const C4HDL_ERROR result = reader(samples->data(), &capacity);
        if (result == C4HDL_ERR_SUCCESS) {
            if (capacity != expectedSamples * sizeof(Sample)) {
                if (errorMessage) {
                    *errorMessage = std::string(operation) + " returned " + std::to_string(capacity)
                        + " byte(s), but the part geometry requires "
                        + std::to_string(expectedSamples * sizeof(Sample)) + " byte(s).";
                }
                return false;
            }
            samples->resize(expectedSamples);
            return true;
        }
        if (result != C4HDL_ERR_SMALL_BUFFER || capacity == 0) {
            if (errorMessage) *errorMessage = operationError(operation, result);
            return false;
        }
        logInfo(std::string(operation) + " requested receive capacity "
            + std::to_string(capacity) + " byte(s); retrying with the SDK-required size.");
    }
    if (errorMessage) {
        *errorMessage = std::string(operation) + " still reports an insufficient buffer after retrying with the SDK-required size.";
    }
    return false;
}

/** Reads one part's complete dimension vector with optional diagnostic output. */
bool readBufferPartDimensions(
    const C4_BUFFER buffer,
    const std::int64_t partIndex,
    const bool detailedDiagnostics,
    std::vector<std::int64_t>* dimensions,
    std::string* errorMessage)
{
    std::uint32_t dimensionBytes = 2 * static_cast<std::uint32_t>(sizeof(std::int64_t));
    dimensions->resize(dimensionBytes / sizeof(std::int64_t));
    C4HDL_ERROR result = C4Buf_getPartDimension(buffer, partIndex, dimensions->data(), &dimensionBytes);
    if (result == C4HDL_ERR_SMALL_BUFFER && dimensionBytes != 0) {
        if (dimensionBytes % sizeof(std::int64_t) != 0) {
            if (errorMessage) *errorMessage = "C4Utility returned a non-integral byte size for buffer dimensions.";
            return false;
        }
        logInfo("C4Buf_getPartDimension part " + std::to_string(partIndex)
            + " requested " + std::to_string(dimensionBytes) + " byte(s); retrying.");
        dimensions->resize(dimensionBytes / sizeof(std::int64_t));
        result = C4Buf_getPartDimension(buffer, partIndex, dimensions->data(), &dimensionBytes);
    }
    if (!checkBufferOperation(result, "C4Buf_getPartDimension", errorMessage)) return false;
    if (dimensionBytes % sizeof(std::int64_t) != 0) {
        if (errorMessage) *errorMessage = "C4Utility returned a non-integral byte size for buffer dimensions.";
        return false;
    }
    dimensions->resize(dimensionBytes / sizeof(std::int64_t));
    if (dimensions->size() < 2) {
        if (errorMessage) *errorMessage = "C4Utility returned fewer than two dimensions for a buffer part.";
        return false;
    }

    if (detailedDiagnostics) {
        std::ostringstream values;
        for (std::size_t index = 0; index < dimensions->size(); ++index) {
            if (index != 0) values << 'x';
            values << dimensions->at(index);
        }
        logInfo("C4Utility buffer part " + std::to_string(partIndex) + " dimensions=" + values.str() + ".");
    }
    return true;
}

template <typename Sample>
std::string sampleDiagnostics(const std::vector<Sample>& samples)
{
    std::size_t finiteCount = 0;
    std::size_t zeroCount = 0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (const Sample sample : samples) {
        const double value = static_cast<double>(sample);
        if (!std::isfinite(value)) continue;
        ++finiteCount;
        if (value == 0.0) ++zeroCount;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }

    std::ostringstream summary;
    summary << "samples=" << samples.size()
            << ", finite=" << finiteCount
            << ", zero=" << zeroCount;
    if (finiteCount != 0U) {
        summary << std::setprecision(12)
                << ", min=" << minimum
                << ", max=" << maximum
                << ", first=" << static_cast<double>(samples.front())
                << ", middle=" << static_cast<double>(samples.at(samples.size() / 2U))
                << ", last=" << static_cast<double>(samples.back());
    }
    return summary.str();
}

bool check(const C4HDL_ERROR result, std::string* errorMessage)
{
    if (result == C4HDL_ERR_SUCCESS) return true;
    if (errorMessage) {
        *errorMessage = "C4Utility code " + std::to_string(static_cast<long long>(result))
            + ": " + lastSdkError();
    }
    return false;
}

void validateC4UtilityRootEnvironment(const std::filesystem::path& expectedRoot)
{
    const char* configuredRoot = std::getenv("C4UTILITY_ROOT");
    if (!configuredRoot || configuredRoot[0] == '\0') {
        throw std::runtime_error(
            "C4UTILITY_ROOT is not configured. The host must point it to the "
            "C4Utility runtime root with a trailing directory separator.");
    }

    const std::string configuredValue(configuredRoot);
    if (configuredValue.back() != '/' && configuredValue.back() != '\\') {
        throw std::runtime_error("C4UTILITY_ROOT must end with a directory separator.");
    }

    std::error_code equivalentError;
    const bool matchesExpectedRoot = std::filesystem::equivalent(
        expectedRoot,
        std::filesystem::path(configuredValue),
        equivalentError);
    if (!matchesExpectedRoot || equivalentError) {
        throw std::runtime_error(
            "C4UTILITY_ROOT does not match the requested C4Utility runtime root. Expected: "
            + internal::C4UtilitySdkRuntime::processRootEnvironmentValue(expectedRoot)
            + ", actual: " + configuredValue);
    }
}

class C4HandlerGuard final {
public:
    explicit C4HandlerGuard(const C4_HANDLER handler)
        : _handler(handler)
    {
    }

    ~C4HandlerGuard()
    {
        if (!_handler) return;
        try {
            C4Hdl_close(_handler);
        } catch (...) {
        }
    }

    C4HandlerGuard(const C4HandlerGuard&) = delete;
    C4HandlerGuard& operator=(const C4HandlerGuard&) = delete;

    [[nodiscard]] C4_HANDLER release() noexcept
    {
        const C4_HANDLER handler = _handler;
        _handler = nullptr;
        return handler;
    }

private:
    C4_HANDLER _handler = nullptr;
};

std::mutex& diagnosticLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

void logInfo(const std::string& message)
{
    std::lock_guard<std::mutex> lock(diagnosticLogMutex());
    std::cout << "[Heliotis C4] " << message << std::endl;
}

void logWarning(const std::string& message)
{
    std::lock_guard<std::mutex> lock(diagnosticLogMutex());
    std::cerr << "[Heliotis C4] " << message << std::endl;
}

template <typename Reader>
std::string readSdkString(Reader&& reader, std::string* errorMessage)
{
    std::size_t size = 256;
    for (int attempt = 0; attempt != 2; ++attempt) {
        std::vector<char> buffer(size, '\0');
        const C4HDL_ERROR result = reader(buffer.data(), &size);
        if (result == C4HDL_ERR_SUCCESS) return std::string(buffer.data());
        if (result != C4HDL_ERR_SMALL_BUFFER) {
            check(result, errorMessage);
            return {};
        }
        size = std::max<std::size_t>(size + 1, 512);
    }
    if (errorMessage) *errorMessage = "C4Utility returned an invalid string buffer size.";
    return {};
}

FeatureType featureType(const Type_e value)
{
    switch (value) {
    case C4FTR_TYPE_INTEGER: return FeatureType::Integer;
    case C4FTR_TYPE_FLOAT: return FeatureType::Float;
    case C4FTR_TYPE_STRING: return FeatureType::String;
    case C4FTR_TYPE_ENUMERATION: return FeatureType::Enumeration;
    case C4FTR_TYPE_COMMAND: return FeatureType::Command;
    case C4FTR_TYPE_BOOLEAN: return FeatureType::Boolean;
    default: return FeatureType::Unknown;
    }
}

FeatureAccess featureAccess(const AccessMode_e value)
{
    switch (value) {
    case C4FTR_ACCESSMODE_NI: return FeatureAccess::NotImplemented;
    case C4FTR_ACCESSMODE_NA: return FeatureAccess::NotAvailable;
    case C4FTR_ACCESSMODE_RO: return FeatureAccess::ReadOnly;
    case C4FTR_ACCESSMODE_WO: return FeatureAccess::WriteOnly;
    case C4FTR_ACCESSMODE_RW: return FeatureAccess::ReadWrite;
    default: return FeatureAccess::Unknown;
    }
}

bool isMotionFeature(const std::string& category, const std::string& name)
{
    std::string value = category + "/" + name;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find("stage") != std::string::npos
        || value.find("scan") != std::string::npos
        || value.find("motion") != std::string::npos
        || value.find("encoder") != std::string::npos;
}

std::string lowerCase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::vector<std::string> splitEnumEntries(const std::string& entries)
{
    std::vector<std::string> result;
    std::istringstream stream(entries);
    for (std::string entry; std::getline(stream, entry, ';');) {
        entry = trim(std::move(entry));
        if (!entry.empty()) result.push_back(std::move(entry));
    }
    return result;
}

bool isWritable(const FeatureAccess access)
{
    return access == FeatureAccess::ReadWrite || access == FeatureAccess::WriteOnly;
}

bool findFeatureMetadata(
    const C4_DEVICE device,
    const std::string& requestedName,
    FeatureType* type,
    FeatureAccess* access,
    std::string* errorMessage)
{
    C4_FEATUREINFO* featureList = nullptr;
    if (!check(C4Dev_getFeatureList(device, &featureList), errorMessage)) return false;
    if (!featureList) {
        if (errorMessage) *errorMessage = "C4Utility returned an empty feature list.";
        return false;
    }

    bool found = false;
    constexpr std::size_t maximumFeatureCount = 4096;
    for (std::size_t index = 0; index < maximumFeatureCount && featureList[index] != nullptr; ++index) {
        const C4_FEATUREINFO feature = featureList[index];
        std::string metadataError;
        const std::string name = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getName(feature, buffer, size); },
            &metadataError);
        if (!metadataError.empty() || name != requestedName) continue;

        Type_e sdkType = C4FTR_TYPE_UNKNOWN;
        AccessMode_e sdkAccess = C4FTR_ACCESSMODE_UNKNOWN;
        if (C4Ftr_getType(feature, &sdkType) != C4HDL_ERR_SUCCESS
            || C4Ftr_getAccessMode(feature, &sdkAccess) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) *errorMessage = lastSdkError();
            break;
        }
        if (type) *type = featureType(sdkType);
        if (access) *access = featureAccess(sdkAccess);
        found = true;
        break;
    }
    C4Ftr_release(featureList);
    if (!found && errorMessage && errorMessage->empty()) {
        *errorMessage = "Heliotis feature is not available: " + requestedName;
    }
    return found;
}

/** Reads an enumeration while preserving the SDK error separately from its value. */
bool readDeviceEnumerationValue(
    const C4_DEVICE device,
    const char* name,
    std::string* value,
    std::string* errorMessage)
{
    std::string error;
    const std::string readValue = readSdkString(
        [device, name](char* buffer, std::size_t* size) {
            return C4Dev_readEnumeration(device, name, buffer, size);
        },
        &error);
    if (!error.empty()) {
        if (errorMessage) *errorMessage = std::move(error);
        return false;
    }
    if (value) *value = readValue;
    return true;
}

/** Reads an enumeration and formats an unavailable value for diagnostics. */
std::string readDeviceEnumeration(const C4_DEVICE device, const char* name)
{
    std::string value;
    std::string error;
    return readDeviceEnumerationValue(device, name, &value, &error)
        ? value
        : std::string("<unavailable: ") + error + ">";
}

/** Reads a string feature for acquisition diagnostics. */
std::string readDeviceString(const C4_DEVICE device, const char* name)
{
    std::string error;
    const std::string value = readSdkString(
        [device, name](char* buffer, std::size_t* size) {
            return C4Dev_readString(device, name, buffer, size);
        },
        &error);
    return error.empty() ? value : std::string("<unavailable: ") + error + ">";
}

/** Reads a floating-point feature for acquisition diagnostics. */
std::string readDeviceFloat(const C4_DEVICE device, const char* name)
{
    double value = 0.0;
    const C4HDL_ERROR result = C4Dev_readFloat(device, name, &value);
    if (result != C4HDL_ERR_SUCCESS) return "<unavailable: " + operationError("C4Dev_readFloat", result) + ">";
    std::ostringstream formatted;
    formatted << std::setprecision(12) << value;
    return formatted.str();
}

/** Reads an integer feature for acquisition diagnostics. */
std::string readDeviceInteger(const C4_DEVICE device, const char* name)
{
    std::int64_t value = 0;
    const C4HDL_ERROR result = C4Dev_readInteger(device, name, &value);
    return result == C4HDL_ERR_SUCCESS
        ? std::to_string(value)
        : "<unavailable: " + operationError("C4Dev_readInteger", result) + ">";
}

/** Reads the current trigger cursor without changing any selector. */
std::string currentTriggerCursorSummary(const C4_DEVICE device)
{
    return "selector=" + readDeviceEnumeration(device, "TriggerSelector")
        + ", mode=" + readDeviceEnumeration(device, "TriggerMode")
        + ", source=" + readDeviceEnumeration(device, "TriggerSource");
}

/** Reads motion state without changing any device feature. */
std::string motionDeviceStateSummary(const C4_DEVICE device)
{
    return "StageType=" + readDeviceEnumeration(device, "StageType")
        + ", StageInitMode=" + readDeviceEnumeration(device, "StageInitMode")
        + ", StageInitialized=" + readDeviceInteger(device, "StageInitialized")
        + ", StageInMotion=" + readDeviceInteger(device, "StageInMotion")
        + ", Position=" + readDeviceFloat(device, "Position")
        + ", ScanPosition=" + readDeviceFloat(device, "ScanPosition")
        + ", ScanRange=" + readDeviceFloat(device, "ScanRange")
        + ", ScanSpeed=" + readDeviceFloat(device, "ScanSpeed")
        + ", GeneralSpeed=" + readDeviceFloat(device, "GeneralSpeed")
        + ", ScanMode=" + readDeviceEnumeration(device, "ScanMode");
}

/** Reads a non-mutating snapshot of capture, payload, processing, and motion settings. */
std::string deviceConfigurationSnapshotSummary(const C4_DEVICE device)
{
    return "AcquisitionMode=" + readDeviceEnumeration(device, "AcquisitionMode")
        + ", DeviceOperationMode=" + readDeviceEnumeration(device, "DeviceOperationMode")
        + ", UserSetSelector=" + readDeviceEnumeration(device, "UserSetSelector")
        + ", UserSetDefault=" + readDeviceEnumeration(device, "UserSetDefault")
        + ", ComponentCursor={selector=" + readDeviceEnumeration(device, "ComponentSelector")
        + ", enabled=" + readDeviceInteger(device, "ComponentEnable") + "}"
        + ", ChunkModeActive=" + readDeviceInteger(device, "ChunkModeActive")
        + ", ChunkCursor={selector=" + readDeviceEnumeration(device, "ChunkSelector")
        + ", enabled=" + readDeviceInteger(device, "ChunkEnable") + "}"
        + ", PayloadSize=" + readDeviceInteger(device, "PayloadSize")
        + ", Scan3dExtractionMethod=" + readDeviceEnumeration(device, "Scan3dExtractionMethod")
        + ", Scan3dScalingMethod=" + readDeviceEnumeration(device, "Scan3dScalingMethod")
        + ", Scan3dDistanceUnit=" + readDeviceEnumeration(device, "Scan3dDistanceUnit")
        + ", TargetVerticalSpacing=" + readDeviceFloat(device, "TargetVerticalSpacing")
        + ", ExposureRatio=" + readDeviceFloat(device, "ExposureRatio")
        + ", Motion={" + motionDeviceStateSummary(device) + "}"
        + ", CurrentTrigger={" + currentTriggerCursorSummary(device) + "}";
}

using internal::TriggerSelectorState;

/** Holds every acquisition trigger gate plus the selector cursor to restore. */
struct TriggerConfigurationSnapshot final {
    bool selectorReadable = false;
    bool restoreSucceeded = true;
    std::string selectedSelector;
    TriggerSelectorState selected;
    TriggerSelectorState acquisitionStart;
    TriggerSelectorState frameStart;
    TriggerSelectorState recordingStart;
    std::string restoreError;
};

/** Reads the mode/source pair at the device's current TriggerSelector cursor. */
TriggerSelectorState readTriggerSelectorState(const C4_DEVICE device)
{
    TriggerSelectorState state;
    std::string modeError;
    std::string sourceError;
    state.modeReadable = readDeviceEnumerationValue(device, "TriggerMode", &state.mode, &modeError);
    state.sourceReadable = readDeviceEnumerationValue(device, "TriggerSource", &state.source, &sourceError);
    // Some firmware makes TriggerSource unavailable while TriggerMode is Off.
    // The source is irrelevant in that state, so a readable Off mode is enough
    // to identify the selector as a free-running/non-triggering stage.
    if (!state.modeReadable) state.error = "TriggerMode: " + modeError;
    if (!state.sourceReadable) {
        if (!state.error.empty()) state.error += "; ";
        state.error += "TriggerSource: " + sourceError;
    }
    return state;
}

/**
 * Reads the selected, AcquisitionStart, FrameStart, and RecordingStart states.
 *
 * The selector is temporarily changed only to inspect selector-dependent
 * mode/source values and is restored before the caller starts acquisition.
 */
TriggerConfigurationSnapshot readTriggerConfigurationSnapshot(const C4_DEVICE device)
{
    TriggerConfigurationSnapshot snapshot;
    std::string selectorError;
    snapshot.selectorReadable = readDeviceEnumerationValue(
        device, "TriggerSelector", &snapshot.selectedSelector, &selectorError);
    if (!snapshot.selectorReadable) {
        snapshot.selected.error = "TriggerSelector: " + selectorError;
        return snapshot;
    }

    snapshot.selected = readTriggerSelectorState(device);
    const std::string originalSelector = snapshot.selectedSelector;
    const auto readForSelector = [device](const char* selector, TriggerSelectorState* state) {
        const C4HDL_ERROR selectResult = C4Dev_writeEnumeration(device, "TriggerSelector", selector);
        if (selectResult != C4HDL_ERR_SUCCESS) {
            state->error = operationError("C4Dev_writeEnumeration(TriggerSelector)", selectResult);
            return;
        }
        *state = readTriggerSelectorState(device);
    };

    readForSelector("AcquisitionStart", &snapshot.acquisitionStart);
    readForSelector("FrameStart", &snapshot.frameStart);
    readForSelector("RecordingStart", &snapshot.recordingStart);

    const C4HDL_ERROR restoreResult = C4Dev_writeEnumeration(
        device, "TriggerSelector", originalSelector.c_str());
    if (restoreResult != C4HDL_ERR_SUCCESS) {
        snapshot.restoreSucceeded = false;
        snapshot.restoreError = operationError("C4Dev_writeEnumeration(TriggerSelector restore)", restoreResult);
    }
    return snapshot;
}

/** Formats one selector state without hiding an unavailable source. */
std::string triggerSelectorStateSummary(const TriggerSelectorState& state)
{
    if (!state.modeReadable) return "unavailable=" + state.error;
    return "mode=" + state.mode + ",source="
        + (state.sourceReadable ? state.source : "<unavailable>")
        + (!state.error.empty() ? ",detail=" + state.error : std::string());
}

/** Formats all trigger gates and selector restoration state for one log record. */
std::string triggerConfigurationSnapshotSummary(const TriggerConfigurationSnapshot& snapshot)
{
    std::ostringstream summary;
    summary << "selected=" << (snapshot.selectorReadable ? snapshot.selectedSelector : "<unavailable>")
            << " {" << triggerSelectorStateSummary(snapshot.selected) << "}"
            << ", AcquisitionStart{" << triggerSelectorStateSummary(snapshot.acquisitionStart) << "}"
            << ", FrameStart{" << triggerSelectorStateSummary(snapshot.frameStart) << "}"
            << ", RecordingStart{" << triggerSelectorStateSummary(snapshot.recordingStart) << "}";
    if (!snapshot.restoreSucceeded) summary << ", selectorRestoreFailed=" << snapshot.restoreError;
    return summary.str();
}

/** Reads every selector-dependent acquisition status and restores its cursor. */
std::string acquisitionStatusSnapshotSummary(const C4_DEVICE device)
{
    std::string originalSelector;
    std::string selectorError;
    if (!readDeviceEnumerationValue(
            device, "AcquisitionStatusSelector", &originalSelector, &selectorError)) {
        return "selector=<unavailable: " + selectorError + ">";
    }

    static constexpr std::array<const char*, 4> selectors{{
        "FrameTriggerWait",
        "RecordingTriggerWait",
        "FrameActive",
        "RecordingActive",
    }};
    std::ostringstream summary;
    summary << "selected=" << originalSelector;
    for (const char* selector : selectors) {
        const C4HDL_ERROR selectResult = C4Dev_writeEnumeration(
            device, "AcquisitionStatusSelector", selector);
        summary << ", " << selector << "=";
        if (selectResult != C4HDL_ERR_SUCCESS) {
            summary << "<unavailable: "
                    << operationError("C4Dev_writeEnumeration(AcquisitionStatusSelector)", selectResult)
                    << ">";
            continue;
        }
        summary << readDeviceInteger(device, "AcquisitionStatus");
    }

    const C4HDL_ERROR restoreResult = C4Dev_writeEnumeration(
        device, "AcquisitionStatusSelector", originalSelector.c_str());
    if (restoreResult != C4HDL_ERR_SUCCESS) {
        summary << ", selectorRestore=<failed: "
                << operationError("C4Dev_writeEnumeration(AcquisitionStatusSelector restore)", restoreResult)
                << ">";
    }
    return summary.str();
}

/** Reads state that distinguishes a trigger wait, motion wait, and re-arm issue. */
std::string acquisitionDeviceStateSummary(const C4_DEVICE device)
{
    return deviceConfigurationSnapshotSummary(device)
        + ", RecordingActive=" + readDeviceInteger(device, "RecordingActive")
        + ", AcquisitionStatuses={" + acquisitionStatusSnapshotSummary(device) + "}"
        + ", LastErrorMessage=" + readDeviceString(device, "LastErrorMessage");
}

FramePartKind framePartKind(const std::string& partType)
{
    const std::string value = lowerCase(partType);
    if (value.find("surface") != std::string::npos || value.find("range") != std::string::npos) {
        return FramePartKind::Range;
    }
    if (value.find("amplitude") != std::string::npos
        || value.find("intensity") != std::string::npos
        || value.find("reflectance") != std::string::npos) {
        return FramePartKind::Intensity;
    }
    if (value.find("confidence") != std::string::npos) {
        return FramePartKind::Confidence;
    }
    return FramePartKind::Unknown;
}

bool usesFloatingPointSamples(const std::string& pixelFormat)
{
    const std::string value = lowerCase(pixelFormat);
    return value.find("float") != std::string::npos
        || value.find("32f") != std::string::npos
        || value.find("64f") != std::string::npos;
}

/** Returns the source payload bit depth without conflating SDK conversion storage. */
std::uint8_t sourceBitsPerSample(const std::string& pixelFormat)
{
    const std::string value = lowerCase(pixelFormat);
    if (value.find("mono8") != std::string::npos) return 8;
    if (value.find("mono16") != std::string::npos) return 16;
    if (value.find("32f") != std::string::npos) return 32;
    if (value.find("64f") != std::string::npos) return 64;
    return 0;
}

/** Writes and reads back one enumeration so SDK cursor/state drift is visible. */
bool writeAndVerifyEnumeration(
    const C4_DEVICE device,
    const char* name,
    const std::string& requestedValue,
    std::string* errorMessage)
{
    const C4HDL_ERROR writeResult = C4Dev_writeEnumeration(device, name, requestedValue.c_str());
    if (writeResult != C4HDL_ERR_SUCCESS) {
        if (errorMessage) *errorMessage = operationError("C4Dev_writeEnumeration", writeResult)
            + " [feature=" + name + ", requested=" + requestedValue + "]";
        return false;
    }

    std::string readback;
    std::string readError;
    if (!readDeviceEnumerationValue(device, name, &readback, &readError)) {
        if (errorMessage) *errorMessage = "Could not verify " + std::string(name)
            + "=" + requestedValue + ": " + readError;
        return false;
    }
    if (readback != requestedValue) {
        if (errorMessage) *errorMessage = "C4Utility did not retain " + std::string(name)
            + "=" + requestedValue + "; readback=" + readback + ".";
        return false;
    }
    return true;
}

/** Executes TriggerSoftware against FrameStart and restores the selector cursor. */
bool executeFrameStartSoftwareTrigger(const C4_DEVICE device, std::string* errorMessage)
{
    std::string originalSelector;
    std::string selectorError;
    if (!readDeviceEnumerationValue(device, "TriggerSelector", &originalSelector, &selectorError)) {
        if (errorMessage) *errorMessage = "Could not read TriggerSelector before TriggerSoftware: " + selectorError;
        return false;
    }

    const bool selectorChanged = originalSelector != "FrameStart";
    if (selectorChanged) {
        const C4HDL_ERROR selectResult = C4Dev_writeEnumeration(device, "TriggerSelector", "FrameStart");
        if (selectResult != C4HDL_ERR_SUCCESS) {
            if (errorMessage) *errorMessage = operationError(
                "C4Dev_writeEnumeration(TriggerSelector=FrameStart)", selectResult);
            return false;
        }
    }

    const C4HDL_ERROR triggerResult = C4Dev_executeCommand(device, "TriggerSoftware");
    std::string commandError;
    if (triggerResult != C4HDL_ERR_SUCCESS) {
        commandError = operationError("C4Dev_executeCommand(TriggerSoftware)", triggerResult);
    }

    std::string restoreError;
    if (selectorChanged) {
        const C4HDL_ERROR restoreResult = C4Dev_writeEnumeration(
            device, "TriggerSelector", originalSelector.c_str());
        if (restoreResult != C4HDL_ERR_SUCCESS) {
            restoreError = operationError("C4Dev_writeEnumeration(TriggerSelector restore)", restoreResult);
        }
    }

    if (!commandError.empty() || !restoreError.empty()) {
        if (errorMessage) {
            *errorMessage = commandError;
            if (!restoreError.empty()) {
                if (!errorMessage->empty()) *errorMessage += "; ";
                *errorMessage += restoreError;
            }
        }
        return false;
    }
    return true;
}

/** Validates TriggerSoftware access while the selector cursor is FrameStart. */
bool validateFrameStartSoftwareTrigger(const C4_DEVICE device, std::string* errorMessage)
{
    std::string originalSelector;
    std::string selectorError;
    if (!readDeviceEnumerationValue(device, "TriggerSelector", &originalSelector, &selectorError)) {
        if (errorMessage) *errorMessage = "Could not read TriggerSelector before validating TriggerSoftware: "
            + selectorError;
        return false;
    }

    const bool selectorChanged = originalSelector != "FrameStart";
    if (selectorChanged) {
        const C4HDL_ERROR selectResult = C4Dev_writeEnumeration(device, "TriggerSelector", "FrameStart");
        if (selectResult != C4HDL_ERR_SUCCESS) {
            if (errorMessage) *errorMessage = operationError(
                "C4Dev_writeEnumeration(TriggerSelector=FrameStart)", selectResult);
            return false;
        }
    }

    FeatureType triggerType = FeatureType::Unknown;
    FeatureAccess triggerAccess = FeatureAccess::Unknown;
    std::string metadataError;
    const bool metadataAvailable = findFeatureMetadata(
        device, "TriggerSoftware", &triggerType, &triggerAccess, &metadataError);

    std::string restoreError;
    if (selectorChanged) {
        const C4HDL_ERROR restoreResult = C4Dev_writeEnumeration(
            device, "TriggerSelector", originalSelector.c_str());
        if (restoreResult != C4HDL_ERR_SUCCESS) {
            restoreError = operationError("C4Dev_writeEnumeration(TriggerSelector restore)", restoreResult);
        }
    }

    if (!metadataAvailable || triggerType != FeatureType::Command || !isWritable(triggerAccess)
        || !restoreError.empty()) {
        if (errorMessage) {
            *errorMessage = !metadataAvailable
                ? metadataError
                : (triggerType != FeatureType::Command || !isWritable(triggerAccess)
                    ? std::string("TriggerSoftware is not an executable FrameStart command.")
                    : std::string());
            if (!restoreError.empty()) {
                if (!errorMessage->empty()) *errorMessage += "; ";
                *errorMessage += restoreError;
            }
        }
        return false;
    }
    return true;
}

/** Distinguishes the C API's generic timed wait result from an immediate fault. */
bool isLikelyTimeoutError(
    const std::string& message,
    const C4HDL_ERROR result,
    const std::chrono::milliseconds elapsed,
    const std::chrono::milliseconds requestedTimeout)
{
    const std::string value = lowerCase(message);
    if (value.find("timeout") != std::string::npos
        || value.find("timed out") != std::string::npos) {
        return true;
    }

    // The C API exposes only a generic error code for getBuffer().  Treat a
    // generic error that consumed the requested wait as a timeout so an armed
    // external/stage trigger remains armed instead of ending acquisition.
    const bool genericError = result == C4HDL_ERR_ERROR
        || value.empty()
        || value == "unknown c4utility error."
        || value == "error";
    constexpr auto tolerance = std::chrono::milliseconds(25);
    return genericError && elapsed + tolerance >= requestedTimeout;
}

bool readBufferString(const C4_BUFFER buffer, const char* name, std::string* value, std::string* errorMessage)
{
    std::string readError;
    const std::string result = readSdkString(
        [buffer, name](char* text, std::size_t* size) {
            return C4Buf_readString(buffer, name, text, size);
        },
        &readError);
    if (!readError.empty() || result.empty()) {
        if (errorMessage) *errorMessage = "C4Utility buffer is missing " + std::string(name) + " metadata.";
        return false;
    }
    *value = result;
    return true;
}

bool readScan3dGeometry(const C4_BUFFER buffer, Scan3dGeometry* geometry, std::string* errorMessage)
{
    if (!buffer || !geometry) return false;

    Scan3dGeometry copied;
    if (!readBufferString(buffer, "ChunkScan3dDistanceUnit", &copied.distanceUnit, errorMessage)
        || !readBufferString(buffer, "ChunkScan3dOutputMode", &copied.outputMode, errorMessage)) {
        return false;
    }

    struct AxisValues {
        const char* selector;
        double* scale;
        double* offset;
    };
    const auto readAxis = [buffer, errorMessage](const AxisValues& axis) {
        if (C4Buf_writeString(buffer, "ChunkScan3dCoordinateSelector", axis.selector) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateScale", axis.scale) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateOffset", axis.offset) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer is missing Scan3d chunk geometry for "
                    + std::string(axis.selector) + ".";
            }
            return false;
        }
        if (!std::isfinite(*axis.scale) || !std::isfinite(*axis.offset)) {
            if (errorMessage) *errorMessage = "C4Utility returned invalid Scan3d chunk geometry for "
                + std::string(axis.selector) + ".";
            return false;
        }
        return true;
    };

    const AxisValues coordinateC{"CoordinateC", &copied.zScale, &copied.zOffset};
    if (!readAxis(coordinateC)) return false;

    const std::string outputMode = lowerCase(copied.outputMode);
    if (outputMode == "rectifiedc") {
        const AxisValues coordinateA{"CoordinateA", &copied.xScale, &copied.xOffset};
        const AxisValues coordinateB{"CoordinateB", &copied.yScale, &copied.yOffset};
        if (!readAxis(coordinateA) || !readAxis(coordinateB)) return false;
    } else if (outputMode != "calibratedc") {
        if (errorMessage) *errorMessage = "C4Utility returned an unsupported Scan3d output mode: " + copied.outputMode;
        return false;
    }

    *geometry = std::move(copied);
    return true;
}

std::string readFeatureValue(const C4_FEATUREINFO feature, const FeatureType type)
{
    if (type == FeatureType::Command || type == FeatureType::Unknown) return {};
    if (type == FeatureType::Integer || type == FeatureType::Boolean) {
        std::int64_t value = 0;
        return C4Ftr_readInteger(feature, &value) == C4HDL_ERR_SUCCESS
            ? std::to_string(value)
            : std::string("<unavailable>");
    }
    if (type == FeatureType::Float) {
        double value = 0.0;
        if (C4Ftr_readFloat(feature, &value) != C4HDL_ERR_SUCCESS) return "<unavailable>";
        std::ostringstream stream;
        stream << std::setprecision(12) << value;
        return stream.str();
    }
    std::string error;
    const std::string value = readSdkString(
        [feature, type](char* buffer, std::size_t* size) {
            return type == FeatureType::Enumeration
                ? C4Ftr_readString(feature, buffer, size)
                : C4Ftr_readString(feature, buffer, size);
        },
        &error);
    return error.empty() ? value : std::string("<unavailable>");
}

} // namespace

HeliotisC4System::HeliotisC4System(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) throw std::runtime_error("C4Utility SDK root is empty.");
    const auto layout = internal::C4UtilitySdkRuntime::fromRoot(sdkRoot);
    if (!layout.isRuntimeComplete()) throw std::runtime_error(layout.runtimeDiagnostic());
    validateC4UtilityRootEnvironment(layout.root);

    C4_HANDLER openedHandler = nullptr;
    const C4HDL_ERROR openResult = C4Hdl_open(&openedHandler);
    C4HandlerGuard handlerGuard(openedHandler);
    std::string error;
    if (!check(openResult, &error)) {
        throw std::runtime_error(error);
    }

    std::string locationError;
    const std::string diaphusLocation = readSdkString(
        [openedHandler](char* buffer, std::size_t* size) {
            return C4Hdl_getDiaphusLocation(openedHandler, buffer, size);
        },
        &locationError);
    std::error_code equivalentError;
    const bool usesRequestedProducer = locationError.empty()
        && !diaphusLocation.empty()
        && std::filesystem::equivalent(
            layout.diaphusProducer,
            std::filesystem::path(diaphusLocation),
            equivalentError);
    if (!usesRequestedProducer || equivalentError) {
        throw std::runtime_error(
            "C4Utility loaded an unexpected Diaphus producer. Expected: "
            + layout.diaphusProducer.string() + ", actual: "
            + (diaphusLocation.empty() ? std::string("<unavailable>") : diaphusLocation)
            + (locationError.empty() ? std::string() : ". C4Utility: " + locationError));
    }
    logInfo("C4Utility handler opened with requested Diaphus producer " + diaphusLocation + ".");
    _handler = handlerGuard.release();
}

HeliotisC4System::~HeliotisC4System()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_handler) {
        C4Hdl_close(_handler);
        _handler = nullptr;
    }
}

std::vector<DeviceDescriptor> HeliotisC4System::discoverDevices(std::string* errorMessage)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DeviceDescriptor> devices;
    if (!_handler) {
        if (errorMessage) *errorMessage = "C4Utility handler is not open.";
        return devices;
    }

    std::int64_t interfaceCount = 0;
    if (!check(C4Hdl_updateInterfaceList(_handler, &interfaceCount), errorMessage)) {
        logWarning("Interface discovery failed: " + (errorMessage ? *errorMessage : lastSdkError()));
        return devices;
    }
    logInfo("Discovery found " + std::to_string(interfaceCount) + " C4 interface(s).");

    for (std::int64_t interfaceIndex = 0; interfaceIndex < interfaceCount; ++interfaceIndex) {
        const std::string interfaceName = readSdkString(
            [this, interfaceIndex](char* buffer, std::size_t* size) {
                return C4Hdl_getInterfaceName(_handler, interfaceIndex, buffer, size);
            },
            errorMessage);
        if (interfaceName.empty() && errorMessage && !errorMessage->empty()) return {};

        C4_INTERFACE interfaceHandle = nullptr;
        if (!check(C4Hdl_openInterface(_handler, &interfaceHandle, interfaceIndex), errorMessage)) return {};

        std::int64_t deviceCount = 0;
        const bool updated = check(C4If_updateDeciveList(interfaceHandle, &deviceCount), errorMessage);
        if (!updated) {
            C4If_release(interfaceHandle);
            return {};
        }

        for (std::int64_t deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
            const std::string deviceName = readSdkString(
                [interfaceHandle, deviceIndex](char* buffer, std::size_t* size) {
                    return C4If_getDeviceName(interfaceHandle, deviceIndex, buffer, size);
                },
                errorMessage);
            if (deviceName.empty() && errorMessage && !errorMessage->empty()) {
                C4If_release(interfaceHandle);
                return {};
            }
            devices.push_back({interfaceIndex, deviceIndex, interfaceName, deviceName});
            logInfo("Discovered device [interface=" + std::to_string(interfaceIndex)
                + ", device=" + std::to_string(deviceIndex) + "]: " + deviceName + ".");
        }
        C4If_release(interfaceHandle);
    }
    logInfo("Discovery completed with " + std::to_string(devices.size()) + " Heliotis device(s).");
    return devices;
}

std::unique_ptr<HeliotisC4Device> HeliotisC4System::createDevice()
{
    return std::make_unique<HeliotisC4Device>(this);
}

HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system)
    : _system(system)
{
}

HeliotisC4Device::~HeliotisC4Device()
{
    close();
}

bool HeliotisC4Device::open(const DeviceDescriptor& descriptor, std::string* errorMessage)
{
    std::lock_guard<std::recursive_mutex> lifecycleLock(_connectionLifecycleMutex);
    close();
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        if (_device) {
            if (errorMessage) {
                *errorMessage = "The previous Heliotis connection is still closing; retry open after its acquisition callback completes.";
            }
            return false;
        }
    }
    logInfo("Opening device [interface=" + std::to_string(descriptor.interfaceIndex)
        + ", device=" + std::to_string(descriptor.deviceIndex) + "]: " + descriptor.deviceName + ".");
    if (!_system) {
        if (errorMessage) *errorMessage = "C4Utility system is unavailable.";
        return false;
    }
    if (descriptor.interfaceIndex < 0 || descriptor.deviceIndex < 0) {
        if (errorMessage) *errorMessage = "The selected Heliotis device has an invalid interface or device index.";
        return false;
    }

    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    {
        std::lock_guard<std::mutex> systemLock(_system->_mutex);
        if (!_system->_handler) {
            if (errorMessage) *errorMessage = "C4Utility handler is not open.";
            return false;
        }
        std::int64_t interfaceCount = 0;
        if (!check(C4Hdl_updateInterfaceList(_system->_handler, &interfaceCount), errorMessage)) return false;
        logInfo("Device open refreshed " + std::to_string(interfaceCount) + " C4 interface(s).");
        if (descriptor.interfaceIndex >= interfaceCount) {
            if (errorMessage) {
                *errorMessage = "The selected Heliotis interface is no longer present in the refreshed interface list.";
            }
            return false;
        }
        std::string interfaceNameError;
        const std::string currentInterfaceName = readSdkString(
            [this, &descriptor](char* buffer, std::size_t* size) {
                return C4Hdl_getInterfaceName(
                    _system->_handler, descriptor.interfaceIndex, buffer, size);
            },
            &interfaceNameError);
        if (!descriptor.interfaceName.empty()
            && (!interfaceNameError.empty() || currentInterfaceName != descriptor.interfaceName)) {
            if (errorMessage) {
                *errorMessage = "The selected Heliotis interface identity changed after discovery; "
                    "refresh the device list before connecting. expected=" + descriptor.interfaceName
                    + ", current=" + (currentInterfaceName.empty() ? "<unavailable>" : currentInterfaceName)
                    + (interfaceNameError.empty() ? std::string() : ", error=" + interfaceNameError) + ".";
            }
            return false;
        }
        if (!check(C4Hdl_openInterface(_system->_handler, &interfaceHandle, descriptor.interfaceIndex), errorMessage)) return false;
        std::int64_t deviceCount = 0;
        if (!check(C4If_updateDeciveList(interfaceHandle, &deviceCount), errorMessage)) {
            C4If_release(interfaceHandle);
            return false;
        }
        if (descriptor.deviceIndex >= deviceCount) {
            if (errorMessage) {
                *errorMessage = "The selected Heliotis device is no longer present in the refreshed interface device list.";
            }
            C4If_release(interfaceHandle);
            return false;
        }
        std::string deviceNameError;
        const std::string currentDeviceName = readSdkString(
            [interfaceHandle, &descriptor](char* buffer, std::size_t* size) {
                return C4If_getDeviceName(interfaceHandle, descriptor.deviceIndex, buffer, size);
            },
            &deviceNameError);
        if (!descriptor.deviceName.empty()
            && (!deviceNameError.empty() || currentDeviceName != descriptor.deviceName)) {
            if (errorMessage) {
                *errorMessage = "The selected Heliotis device identity changed after discovery; "
                    "refresh the device list before connecting. expected=" + descriptor.deviceName
                    + ", current=" + (currentDeviceName.empty() ? "<unavailable>" : currentDeviceName)
                    + (deviceNameError.empty() ? std::string() : ", error=" + deviceNameError) + ".";
            }
            C4If_release(interfaceHandle);
            return false;
        }
        logInfo("Device identity verified before open: interface="
            + (currentInterfaceName.empty() ? descriptor.interfaceName : currentInterfaceName)
            + ", device=" + (currentDeviceName.empty() ? descriptor.deviceName : currentDeviceName) + ".");
        if (!check(C4If_openDevice(interfaceHandle, &deviceHandle, descriptor.deviceIndex), errorMessage)) {
            C4If_release(interfaceHandle);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _interface = interfaceHandle;
        _device = deviceHandle;
        _connectedDeviceName = descriptor.deviceName;
        _requiresReconnect = false;
        _lastAcquisitionError.clear();
    }
    logInfo("Device open preserved the existing configuration; no capture profile, user set, motion command, or trigger value was applied. Initial snapshot: "
        + deviceConfigurationSnapshotSummary(deviceHandle) + ".");
    dispatchConnectionStatus(true);
    if (!isOpened()) {
        if (errorMessage) {
            *errorMessage = "The Heliotis connection was closed by a connection-status callback during open.";
        }
        logWarning("Device open was cancelled by a reentrant connection-status callback.");
        return false;
    }
    logInfo("Device connection opened: " + descriptor.deviceName + ".");
    return true;
}

void HeliotisC4Device::close()
{
    std::lock_guard<std::recursive_mutex> lifecycleLock(_connectionLifecycleMutex);
    requestCancelInitialization();
    bool callbackThread = false;
    bool initializationThread = false;
    bool initializationPending = false;
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        callbackThread = (_activeStatusDispatching
                && _activeStatusDispatchThreadId == std::this_thread::get_id())
            || (_acquisitionThread.joinable()
                && _acquisitionThread.get_id() == std::this_thread::get_id());
        initializationThread = _initializing
            && _initializationThreadId == std::this_thread::get_id();
        initializationPending = _initializing;
        if (!callbackThread && !initializationThread) _closing = true;
    }
    if (callbackThread || initializationThread) {
        requestStopAcquisition();
        logWarning(initializationThread
            ? "Device close was deferred because an initialization operation cannot release its own device handles."
            : "Device close was deferred because an acquisition callback cannot release its own worker resources.");
        return;
    }
    if (initializationPending) {
        logInfo("Device close requested initialization cancellation and is waiting for the operation to release the SDK channel.");
    }
    {
        std::unique_lock<std::mutex> stateLock(_stateMutex);
        _initializationCondition.wait(stateLock, [this] { return !_initializing; });
    }
    if (initializationPending) {
        logInfo("Device close observed initialization cancellation completion.");
    }
    stopAcquisition();

    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    bool wasOpen = false;
    bool recoveryStopRequired = false;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        wasOpen = _device != nullptr;
        recoveryStopRequired = _requiresReconnect;
        interfaceHandle = _interface;
        deviceHandle = _device;
        _interface = nullptr;
        _device = nullptr;
        _connectedDeviceName.clear();
        _softwareTriggeredAcquisition = false;
        _softwareTriggerAvailable = false;
        _requiresReconnect = false;
        _activeAcquisitionId = 0;
        _lastAcquisitionError.clear();
    }
    const auto releaseStarted = std::chrono::steady_clock::now();
    C4HDL_ERROR recoveryStopResult = C4HDL_ERR_SUCCESS;
    C4HDL_ERROR deviceReleaseResult = C4HDL_ERR_SUCCESS;
    C4HDL_ERROR interfaceReleaseResult = C4HDL_ERR_SUCCESS;
    if (deviceHandle && recoveryStopRequired) {
        recoveryStopResult = C4Dev_stopAcquisition(deviceHandle);
        if (recoveryStopResult == C4HDL_ERR_SUCCESS) {
            logInfo("Close-time C4Dev_stopAcquisition recovery succeeded after the earlier stop failure.");
        } else {
            logWarning("Close-time C4Dev_stopAcquisition recovery failed: "
                + operationError("C4Dev_stopAcquisition", recoveryStopResult));
        }
    }
    if (deviceHandle) {
        deviceReleaseResult = C4Dev_release(deviceHandle);
        if (deviceReleaseResult != C4HDL_ERR_SUCCESS) {
            logWarning("C4Dev_release failed while closing the device: "
                + operationError("C4Dev_release", deviceReleaseResult));
        }
    }
    if (interfaceHandle) {
        interfaceReleaseResult = C4If_release(interfaceHandle);
        if (interfaceReleaseResult != C4HDL_ERR_SUCCESS) {
            logWarning("C4If_release failed while closing the interface: "
                + operationError("C4If_release", interfaceReleaseResult));
        }
    }
    const auto releaseElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - releaseStarted);
    if (wasOpen) {
        logInfo("Device connection closed [elapsedMs=" + std::to_string(releaseElapsed.count())
            + ", recoveryStopCode=" + std::to_string(static_cast<long long>(recoveryStopResult))
            + ", deviceReleaseCode=" + std::to_string(static_cast<long long>(deviceReleaseResult))
            + ", interfaceReleaseCode=" + std::to_string(static_cast<long long>(interfaceReleaseResult))
            + "].");
        dispatchConnectionStatus(false);
    }
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _closing = false;
    }
}

bool HeliotisC4Device::isOpened() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _device != nullptr;
}

std::string HeliotisC4Device::connectedDeviceName() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _connectedDeviceName;
}

bool HeliotisC4Device::configureH8SurfaceExample(std::string* errorMessage)
{
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }
        if (_closing) {
            if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
            return false;
        }
        if (_requiresReconnect) {
            if (errorMessage) {
                *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before H8 initialization.";
            }
            return false;
        }
        if (_acquiring) {
            if (errorMessage) *errorMessage = "The H8 reference profile cannot be applied during acquisition.";
            return false;
        }
        if (_initializing) {
            if (errorMessage) *errorMessage = "H8 reference-profile initialization is already running.";
            return false;
        }
        // Clear a stale request only after this call has won initialization
        // ownership. A rejected concurrent call must not erase cancellation
        // intended for the operation that is already running.
        _cancelInitializationRequested.store(false);
        _initializing = true;
        _initializationThreadId = std::this_thread::get_id();
    }

    const auto clearInitializationState = [this] {
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _initializing = false;
            _initializationThreadId = {};
            _cancelInitializationRequested.store(false);
        }
        _initializationCondition.notify_all();
    };

    bool configured = false;
    try {
        configured = [this, errorMessage]() {
    const auto cancelled = [this, errorMessage](const char* phase) {
        if (!_cancelInitializationRequested.load()) return false;
        const std::string message = std::string("H8 reference-profile initialization was cancelled during ")
            + phase + ".";
        if (errorMessage) *errorMessage = message;
        logWarning(message);
        return true;
    };

    struct FeatureWrite final {
        const char* name;
        const char* value;
    };
    // Keep the measurement/motion setup aligned with C4Utility 1.12's
    // h8SurfSimple configuration. Trigger routing remains device-owned: do
    // not overwrite the user's RecordingStart, AcquisitionStart, or
    // FrameStart configuration during connection.
    static constexpr std::array<FeatureWrite, 20> preStageInitWrites{{
        {"ComponentSelector", "Intensity"}, {"ComponentEnable", "0"},
        {"ComponentSelector", "Range"}, {"ComponentEnable", "1"},
        {"ComponentSelector", "Reflectance"}, {"ComponentEnable", "1"},
        {"ComponentSelector", "Phase"}, {"ComponentEnable", "0"},
        {"ChunkModeActive", "1"},
        {"ChunkSelector", "PartCount"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "PartType"}, {"ChunkEnable", "1"},
        {"EncoderSelector", "Camera"}, {"EncoderInverter", "1"},
        {"ScanPosition", "-1.4"}, {"ScanRange", "0.5"}, {"ScanSpeed", "5.0"},
        {"GeneralSpeed", "10.0"}, {"ScanMode", "Down"},
    }};
    // A Surface payload remains capturable without these physical-coordinate
    // chunks. Enable them as one selector-dependent optional group when the
    // connected firmware exposes the complete contract.
    static constexpr std::array<FeatureWrite, 8> scan3dGeometryChunkWrites{{
        {"ChunkSelector", "Scan3dDistanceUnit"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dOutputMode"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dCoordinateScale"}, {"ChunkEnable", "1"},
        {"ChunkSelector", "Scan3dCoordinateOffset"}, {"ChunkEnable", "1"},
    }};
    static constexpr std::array<FeatureWrite, 8> postStageInitWrites{{
        {"Scan3dExtractionMethod", "AcceleratedCenterOfMassIQCorrection"},
        {"Scan3dScalingMethod", "zTags"}, {"Scan3dDistanceUnit", "um"},
        {"TargetVerticalSpacing", "2.5"}, {"ExposureRatio", "1.0"},
        {"FPNCorrection", "AverageLastFrames"}, {"FPNCorrectionNFrames", "8"},
        {"ExtSimpMaxHWin", "7"},
    }};
    static constexpr std::array<FeatureWrite, 3> lightControllerWrites{{
        {"LightControllerSelector", "LightController0"},
        {"LightControllerSource", "UserOutput0"},
        {"LightBrightness", "100.0"},
    }};
    static constexpr std::array<FeatureWrite, 2> userOutputWrites{{
        {"UserOutputSelector", "UserOutput0"},
        {"UserOutputValue", "1"},
    }};

    bool optionalConfigurationWarning = false;
    const auto applyWrites = [this, errorMessage, &cancelled](
        const auto& steps,
        const char* phase) {
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (cancelled(phase)) return false;
            const auto& step = steps[index];
            logInfo(std::string("H8 reference profile [") + phase + " "
                + std::to_string(index + 1) + "/" + std::to_string(steps.size()) + "]: "
                + step.name + "=" + step.value + ".");
            if (!writeFeature(step.name, step.value, errorMessage)) {
                logWarning(std::string("H8 reference profile failed at ") + step.name + "=" + step.value + ".");
                return false;
            }
        }
        return true;
    };
    const auto applyOptionalGroup = [this, errorMessage, &cancelled, &optionalConfigurationWarning](
        const auto& steps,
        const char* groupName) {
        for (std::size_t index = 0; index < steps.size(); ++index) {
            if (cancelled(groupName)) return false;
            const auto& step = steps[index];
            logInfo(std::string("H8 optional profile [") + groupName + " "
                + std::to_string(index + 1) + "/" + std::to_string(steps.size()) + "]: "
                + step.name + "=" + step.value + ".");
            std::string optionalError;
            if (!writeFeature(step.name, step.value, &optionalError)) {
                optionalConfigurationWarning = true;
                logWarning(std::string("H8 optional group '") + groupName + "' stopped at "
                    + step.name + "=" + step.value + ": "
                    + (optionalError.empty()
                        ? std::string("the device does not expose a writable control")
                        : optionalError)
                    + ". Remaining writes in this selector-dependent group were skipped; capture can continue.");
                return true;
            }
        }
        return true;
    };

    logInfo("Applying C4Utility h8SurfSimple reference profile to the connected H8.");
    if (!applyWrites(preStageInitWrites, "pre-stage-init")) return false;
    if (!applyOptionalGroup(scan3dGeometryChunkWrites, "scan3d-geometry-chunks")) return false;

    logInfo("H8 reference profile: executing StageInit after motion configuration.");
    if (!executeCommand("StageInit", errorMessage)) return false;

    constexpr auto initializationTimeout = std::chrono::seconds(30);
    constexpr auto pollInterval = std::chrono::milliseconds(100);
    const auto deadline = std::chrono::steady_clock::now() + initializationTimeout;
    std::int64_t initialized = 0;
    std::size_t pollCount = 0;
    do {
        std::this_thread::sleep_for(pollInterval);
        if (cancelled("StageInit wait")) return false;
        {
            std::scoped_lock lock(_stateMutex, _sdkMutex);
            if (!_device) {
                if (errorMessage) *errorMessage = "Heliotis C4 device disconnected during reference-profile StageInit.";
                return false;
            }
            if (!check(C4Dev_readInteger(_device, "StageInitialized", &initialized), errorMessage)) return false;
        }
        ++pollCount;
        if (pollCount == 1 || initialized != 0 || pollCount % 10 == 0) {
            logInfo("H8 reference profile: StageInitialized=" + std::to_string(initialized)
                + " (poll " + std::to_string(pollCount) + ").");
        }
        if (initialized != 0) break;
    } while (std::chrono::steady_clock::now() < deadline);
    if (initialized == 0) {
        if (errorMessage) *errorMessage = "H8 reference-profile StageInit did not finish within 30 seconds.";
        logWarning("H8 reference profile: StageInit timed out after 30 seconds.");
        return false;
    }

    if (!applyWrites(postStageInitWrites, "post-stage-init")) return false;
    if (!applyOptionalGroup(lightControllerWrites, "light-controller")) return false;
    if (!applyOptionalGroup(userOutputWrites, "user-output")) return false;
    TriggerConfigurationSnapshot triggerSnapshot;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device disconnected before H8 profile verification.";
            return false;
        }
        triggerSnapshot = readTriggerConfigurationSnapshot(_device);
    }
    const std::string completionSummary = "H8 reference profile completed; required Range/Reflectance, multipart identity chunks, motion, and processing are configured. "
        "Existing trigger configuration is preserved: "
        + triggerConfigurationSnapshotSummary(triggerSnapshot) + ".";
    if (optionalConfigurationWarning) {
        logWarning(completionSummary + " Optional capability configuration has warnings; acquisition remains available.");
    } else {
        logInfo(completionSummary + " Optional Scan3d geometry and illumination capabilities are configured.");
    }
    if (!triggerSnapshot.selectorReadable || !triggerSnapshot.restoreSucceeded) {
        logWarning("H8 trigger readiness could not be verified after profile initialization; acquisition arm will fail closed until all selectors can be inspected and restored.");
    } else {
        const internal::AcquisitionTriggerPlan triggerPlan = internal::evaluateAcquisitionTriggerPlan(
            triggerSnapshot.acquisitionStart,
            triggerSnapshot.frameStart,
            triggerSnapshot.recordingStart);
        if (!triggerPlan.valid) {
            logWarning("H8 profile initialization succeeded, but the preserved trigger configuration is not armable: "
                + triggerPlan.error);
        } else if (!triggerPlan.warning.empty()) {
            logWarning("H8 preserved trigger plan: " + triggerPlan.summary() + ".");
        } else {
            logInfo("H8 preserved trigger plan: " + triggerPlan.summary() + ".");
        }
    }
    return true;
        }();
    } catch (...) {
        clearInitializationState();
        throw;
    }
    clearInitializationState();
    return configured;
}

void HeliotisC4Device::requestCancelInitialization() noexcept
{
    _cancelInitializationRequested.store(true);
}

bool HeliotisC4Device::initializeMotion(std::string* errorMessage)
{
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }
        if (_closing) {
            if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
            return false;
        }
        if (_requiresReconnect) {
            if (errorMessage) {
                *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before motion initialization.";
            }
            return false;
        }
        if (_acquiring) {
            if (errorMessage) *errorMessage = "Heliotis motion cannot be initialized during acquisition.";
            return false;
        }
        if (_initializing) {
            if (errorMessage) *errorMessage = "Another Heliotis initialization operation is already running.";
            return false;
        }
        // Do not let a rejected concurrent motion request clear cancellation
        // for the initialization operation that currently owns the device.
        _cancelInitializationRequested.store(false);
        _initializing = true;
        _initializationThreadId = std::this_thread::get_id();
    }

    const auto clearInitializationState = [this] {
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _initializing = false;
            _initializationThreadId = {};
            _cancelInitializationRequested.store(false);
        }
        _initializationCondition.notify_all();
    };

    bool initializedSuccessfully = false;
    try {
        initializedSuccessfully = [this, errorMessage]() {
            constexpr auto initializationTimeout = std::chrono::seconds(30);
            constexpr auto pollInterval = std::chrono::milliseconds(100);

            std::string initialSnapshot;
            {
                std::scoped_lock lock(_stateMutex, _sdkMutex);
                if (!_device) {
                    if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
                    return false;
                }
                initialSnapshot = deviceConfigurationSnapshotSummary(_device);
            }
            logInfo("Stage Init operation started. Policy: inspect StageInitialized and execute only StageInit when false; no capture-profile or trigger values are written. Initial snapshot: "
                + initialSnapshot + ".");

            const auto cancelled = [this, errorMessage](const char* phase) {
                if (!_cancelInitializationRequested.load()) return false;
                const std::string message = std::string("H8 motion initialization was cancelled during ")
                    + phase + ".";
                if (errorMessage) *errorMessage = message;
                logWarning(message);
                return true;
            };
            const auto readInitialized = [this, errorMessage](std::int64_t* initialized) {
                std::scoped_lock lock(_stateMutex, _sdkMutex);
                if (!_device) {
                    if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
                    return false;
                }
                if (_requiresReconnect) {
                    if (errorMessage) {
                        *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before motion initialization.";
                    }
                    return false;
                }
                if (_acquiring) {
                    if (errorMessage) *errorMessage = "Heliotis acquisition started while motion initialization was running.";
                    return false;
                }
                if (_initializing && _initializationThreadId != std::this_thread::get_id()) {
                    if (errorMessage) *errorMessage = "Another Heliotis initialization operation owns the device.";
                    return false;
                }
                if (!check(C4Dev_readInteger(_device, "StageInitialized", initialized), errorMessage)) {
                    if (errorMessage && !errorMessage->empty()) {
                        *errorMessage = "Could not read H8 StageInitialized: " + *errorMessage;
                    }
                    return false;
                }
                return true;
            };

            if (cancelled("readiness check")) return false;
            std::int64_t initialized = 0;
            if (!readInitialized(&initialized)) {
                logWarning("Stage Init availability check failed. The connection and existing capture configuration remain usable: "
                    + (errorMessage && !errorMessage->empty()
                        ? *errorMessage
                        : std::string("StageInitialized is unavailable")) + ".");
                return false;
            }
            logInfo("Stage Init availability check: StageInitialized=" + std::to_string(initialized) + ".");
            if (initialized != 0) {
                logInfo("Stage Init skipped because motion is already initialized. No device value was changed. State: "
                    + initialSnapshot + ".");
                return true;
            }

            const auto commandStarted = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(_stateMutex, _sdkMutex);
                if (!_device) {
                    if (errorMessage) *errorMessage = "Heliotis C4 device disconnected before motion initialization.";
                    return false;
                }
                if (_cancelInitializationRequested.load()) {
                    if (errorMessage) *errorMessage = "H8 motion initialization was cancelled before StageInit.";
                    return false;
                }
                logInfo("Executing the StageInit command because StageInitialized is false; the stage may move.");
                if (!check(C4Dev_executeCommand(_device, "StageInit"), errorMessage)) {
                    if (errorMessage && !errorMessage->empty()) {
                        *errorMessage = "Could not start H8 StageInit: " + *errorMessage;
                    }
                    logWarning("StageInit command failed; no capture-profile or trigger value was changed. State: "
                        + motionDeviceStateSummary(_device) + ".");
                    return false;
                }
            }

            const auto deadline = std::chrono::steady_clock::now() + initializationTimeout;
            std::size_t pollCount = 0;
            do {
                std::this_thread::sleep_for(pollInterval);
                if (cancelled("StageInit wait")) return false;
                if (!readInitialized(&initialized)) return false;
                ++pollCount;
                if (pollCount == 1 || initialized != 0 || pollCount % 10 == 0) {
                    std::string pollState;
                    {
                        std::scoped_lock lock(_stateMutex, _sdkMutex);
                        if (_device) pollState = motionDeviceStateSummary(_device);
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - commandStarted);
                    logInfo("StageInit progress [poll=" + std::to_string(pollCount)
                        + ", elapsedMs=" + std::to_string(elapsed.count())
                        + "]: " + (pollState.empty() ? std::string("device disconnected") : pollState) + ".");
                }
                if (initialized != 0) {
                    std::string finalSnapshot;
                    {
                        std::scoped_lock lock(_stateMutex, _sdkMutex);
                        if (_device) finalSnapshot = deviceConfigurationSnapshotSummary(_device);
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - commandStarted);
                    logInfo("StageInit completed successfully [elapsedMs="
                        + std::to_string(elapsed.count()) + "]. Final snapshot: "
                        + (finalSnapshot.empty() ? std::string("device disconnected") : finalSnapshot) + ".");
                    return true;
                }
            } while (std::chrono::steady_clock::now() < deadline);

            if (errorMessage) *errorMessage = "H8 StageInit did not finish within 30 seconds.";
            logWarning("StageInit timed out after 30 seconds.");
            return false;
        }();
    } catch (...) {
        clearInitializationState();
        throw;
    }
    clearInitializationState();
    return initializedSuccessfully;
}

HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    HeliotisC4::FeatureList features;
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return features;
    }
    if (_closing) {
        if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
        return features;
    }
    if (_requiresReconnect) {
        if (errorMessage) {
            *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before reading features.";
        }
        return features;
    }
    if (_initializing && _initializationThreadId != std::this_thread::get_id()) {
        if (errorMessage) *errorMessage = "Feature refresh is unavailable while H8 initialization is running.";
        return features;
    }
    if (_acquiring) {
        if (errorMessage) *errorMessage = "Feature refresh is unavailable while Heliotis acquisition is armed.";
        return features;
    }
    C4_FEATUREINFO* featureList = nullptr;
    if (!check(C4Dev_getFeatureList(_device, &featureList), errorMessage)) return features;
    if (!featureList) return features;

    constexpr std::size_t maximumFeatureCount = 4096;
    for (std::size_t index = 0; index < maximumFeatureCount && featureList[index] != nullptr; ++index) {
        const C4_FEATUREINFO feature = featureList[index];
        std::string metadataError;
        const std::string name = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getName(feature, buffer, size); },
            &metadataError);
        if (!metadataError.empty() || name.empty()) continue;

        const std::string category = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getCategory(feature, buffer, size); },
            &metadataError);
        const std::string description = readSdkString(
            [feature](char* buffer, std::size_t* size) { return C4Ftr_getDescription(feature, buffer, size); },
            &metadataError);
        Type_e sdkType = C4FTR_TYPE_UNKNOWN;
        AccessMode_e sdkAccess = C4FTR_ACCESSMODE_UNKNOWN;
        C4Ftr_getType(feature, &sdkType);
        C4Ftr_getAccessMode(feature, &sdkAccess);
        const FeatureType type = featureType(sdkType);
        const FeatureAccess access = featureAccess(sdkAccess);
        if (access == FeatureAccess::NotImplemented) {
            continue;
        }
        std::vector<std::string> enumEntries;
        if (type == FeatureType::Enumeration) {
            std::string enumError;
            enumEntries = splitEnumEntries(readSdkString(
                [feature](char* buffer, std::size_t* size) {
                    return C4Ftr_getEnumEntryList(feature, buffer, size);
                },
                &enumError));
        }
        features.push_back({
            isMotionFeature(category, name) ? FeatureSection::Motion : FeatureSection::Device,
            category,
            name,
            type == FeatureType::Command ? std::string("Execute")
                : (access == FeatureAccess::WriteOnly ? std::string("<write only>")
                    : (access == FeatureAccess::ReadOnly || access == FeatureAccess::ReadWrite
                        ? readFeatureValue(feature, type)
                        : std::string("<unavailable>"))),
            description,
            type,
            access,
            std::move(enumEntries),
        });
    }
    C4Ftr_release(featureList);
    logInfo("Read " + std::to_string(features.size()) + " available feature(s).");
    return features;
}

bool HeliotisC4Device::writeFeature(
    const std::string& name,
    const std::string& value,
    std::string* errorMessage)
{
    const std::string trimmedValue = trim(value);
    if (name.empty() || trimmedValue.empty()) {
        if (errorMessage) *errorMessage = "Heliotis feature name and value are required.";
        return false;
    }

    logInfo("Feature write requested: " + name + "=" + trimmedValue + ".");
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (_closing) {
        if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
        return false;
    }
    if (_requiresReconnect) {
        if (errorMessage) {
            *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before writing features.";
        }
        return false;
    }
    if (_initializing && _initializationThreadId != std::this_thread::get_id()) {
        if (errorMessage) *errorMessage = "Feature writes are unavailable while H8 initialization is running.";
        return false;
    }
    if (_acquiring) {
        if (errorMessage) *errorMessage = "Feature writes are unavailable while Heliotis acquisition is armed.";
        return false;
    }
    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    if (!findFeatureMetadata(_device, name, &type, &access, errorMessage)) return false;
    if (!isWritable(access) || type == FeatureType::Command) {
        if (errorMessage) *errorMessage = "Heliotis feature is not writable: " + name;
        return false;
    }

    C4HDL_ERROR result = C4HDL_ERR_ERROR;
    switch (type) {
    case FeatureType::Integer:
    case FeatureType::Boolean: {
        std::string normalized = lowerCase(trimmedValue);
        if (type == FeatureType::Boolean) {
            if (normalized == "true") normalized = "1";
            if (normalized == "false") normalized = "0";
        }
        char* end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(normalized.c_str(), &end, 10);
        if (errno == ERANGE || end == normalized.c_str() || *end != '\0'
            || (type == FeatureType::Boolean && parsed != 0 && parsed != 1)) {
            if (errorMessage) *errorMessage = "Invalid integer value for Heliotis feature " + name + ".";
            return false;
        }
        result = C4Dev_writeInteger(_device, name.c_str(), static_cast<std::int64_t>(parsed));
        break;
    }
    case FeatureType::Float: {
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(trimmedValue.c_str(), &end);
        if (errno == ERANGE || end == trimmedValue.c_str() || *end != '\0' || !std::isfinite(parsed)) {
            if (errorMessage) *errorMessage = "Invalid floating-point value for Heliotis feature " + name + ".";
            return false;
        }
        result = C4Dev_writeFloat(_device, name.c_str(), parsed);
        break;
    }
    case FeatureType::String:
        result = C4Dev_writeString(_device, name.c_str(), trimmedValue.c_str());
        break;
    case FeatureType::Enumeration:
        result = C4Dev_writeEnumeration(_device, name.c_str(), trimmedValue.c_str());
        break;
    default:
        if (errorMessage) *errorMessage = "Heliotis feature type is not writable: " + name;
        return false;
    }
    const bool succeeded = check(result, errorMessage);
    if (succeeded) {
        logInfo("Feature write completed: " + name + "=" + trimmedValue + ".");
        if (name == "TriggerSelector" || name == "TriggerMode" || name == "TriggerSource") {
            logInfo("Current trigger configuration: "
                + triggerConfigurationSnapshotSummary(readTriggerConfigurationSnapshot(_device)) + ".");
        }
    } else {
        logWarning("Feature write failed for " + name + ": "
            + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
    }
    return succeeded;
}

bool HeliotisC4Device::executeCommand(const std::string& name, std::string* errorMessage)
{
    if (name.empty()) {
        if (errorMessage) *errorMessage = "Heliotis command name is required.";
        return false;
    }

    logInfo("Command requested: " + name + ".");
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
        return false;
    }
    if (_closing) {
        if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
        return false;
    }
    if (_requiresReconnect) {
        if (errorMessage) {
            *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before executing commands.";
        }
        return false;
    }
    if (_initializing && _initializationThreadId != std::this_thread::get_id()) {
        if (errorMessage) *errorMessage = "Commands are unavailable while H8 initialization is running.";
        return false;
    }
    if (_acquiring) {
        if (errorMessage) {
            *errorMessage = "Commands are unavailable while Heliotis acquisition is armed; "
                "use triggerSoftware() for FrameStart.";
        }
        return false;
    }
    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    if (!findFeatureMetadata(_device, name, &type, &access, errorMessage)) return false;
    if (type != FeatureType::Command || !isWritable(access)) {
        if (errorMessage) *errorMessage = "Heliotis command is not executable: " + name;
        return false;
    }
    const bool succeeded = check(C4Dev_executeCommand(_device, name.c_str()), errorMessage);
    if (succeeded) logInfo("Command completed: " + name + ".");
    else logWarning("Command failed for " + name + ": "
        + (errorMessage && !errorMessage->empty() ? *errorMessage : lastSdkError()));
    return succeeded;
}

bool HeliotisC4Device::triggerSoftware(std::string* errorMessage)
{
    std::uint64_t acquisitionId = 0;
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }
        if (_closing) {
            if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
            return false;
        }
        if (_requiresReconnect) {
            if (errorMessage) {
                *errorMessage = "The previous acquisition did not stop cleanly. Disconnect and reconnect before software triggering.";
            }
            logWarning("Software trigger rejected because reconnect is required after an SDK stop failure.");
            return false;
        }
        if (!_acquiring) {
            if (errorMessage) *errorMessage = "Software trigger requires an armed Heliotis acquisition.";
            logWarning("Software trigger rejected because acquisition is not armed.");
            return false;
        }
        if (_stopAcquisitionRequested.load()) {
            if (errorMessage) *errorMessage = "Software trigger is unavailable while Heliotis acquisition is stopping.";
            logWarning("Software trigger rejected because acquisition shutdown is already pending.");
            return false;
        }
        if (!_softwareTriggerAvailable) {
            if (errorMessage) {
                *errorMessage = "Software trigger is unavailable because FrameStart is not configured as On/Software.";
            }
            logWarning("Software trigger rejected because FrameStart is not On/Software.");
            return false;
        }
        acquisitionId = _activeAcquisitionId;
    }

    {
        std::lock_guard<std::mutex> triggerLock(_triggerMutex);
        if (_stopAcquisitionRequested.load()) {
            if (errorMessage) *errorMessage = "Software trigger is unavailable while Heliotis acquisition is stopping.";
            logWarning("Software trigger rejected because acquisition shutdown began before queue insertion.");
            return false;
        }
        if (_pendingSoftwareTriggers != 0 || _softwareTriggerInFlight) {
            if (errorMessage) {
                *errorMessage = "The previous Heliotis software trigger is still waiting for its frame.";
            }
            logWarning("Software trigger rejected because a previous trigger is still in flight.");
            return false;
        }
        ++_pendingSoftwareTriggers;
    }
    _triggerCondition.notify_one();
    logInfo("FrameStart TriggerSoftware command queued [arm="
        + std::to_string(acquisitionId) + "].");
    return true;
}

bool HeliotisC4Device::startAcquisition(
    const AcquisitionMode mode,
    FrameCallback frameCallback,
    std::string* errorMessage)
{
    if (!frameCallback) {
        if (errorMessage) *errorMessage = "Heliotis acquisition requires a frame callback.";
        return false;
    }

    std::unique_lock<std::mutex> armLock(_acquisitionArmMutex, std::try_to_lock);
    if (!armLock.owns_lock()) {
        if (errorMessage) *errorMessage = "Another Heliotis acquisition arm request is still in progress.";
        logWarning("Acquisition arm rejected because another arm request is still in progress.");
        return false;
    }

    stopAcquisition();
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        if (_acquiring || _acquisitionWorkerInstalling || _acquisitionThread.joinable()) {
            if (errorMessage) {
                *errorMessage = "The previous Heliotis acquisition worker is still completing a callback; "
                    "retry after the inactive acquisition status is delivered.";
            }
            logWarning("Acquisition arm rejected because the previous worker could not be joined from its own callback.");
            return false;
        }
    }

    const char* modeName = mode == AcquisitionMode::SingleFrame ? "Single" : "Live";
    logInfo(std::string("Acquisition arm requested: mode=") + modeName + ".");

    C4_DEVICE device = nullptr;
    bool softwareTriggered = false;
    bool softwareTriggerAvailable = false;
    bool restoreAcquisitionMode = false;
    std::string originalAcquisitionMode;
    std::string triggerDiagnosticSummary;
    std::uint64_t acquisitionId = 0;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }
        if (_closing) {
            if (errorMessage) *errorMessage = "Heliotis C4 device shutdown is in progress.";
            return false;
        }
        if (_initializing) {
            if (errorMessage) *errorMessage = "Heliotis acquisition cannot start while H8 initialization is running.";
            return false;
        }
        if (_requiresReconnect) {
            if (errorMessage) {
                *errorMessage = "The previous Heliotis acquisition did not stop cleanly. "
                    "Disconnect and reconnect the device before re-arming.";
            }
            logWarning("Acquisition arm blocked because the previous SDK stop failed; reconnect is required.");
            return false;
        }

        device = _device;
        acquisitionId = _nextAcquisitionId++;
        const std::string existingConfiguration = deviceConfigurationSnapshotSummary(device);
        logInfo("Acquisition pre-arm snapshot [arm=" + std::to_string(acquisitionId)
            + ", requestedMode=" + modeName + "]: " + existingConfiguration + ".");
        const std::string chunkModeActive = readDeviceInteger(device, "ChunkModeActive");
        if (chunkModeActive != "1") {
            logWarning("ChunkModeActive is " + chunkModeActive + " before acquisition [arm="
                + std::to_string(acquisitionId)
                + "]. Arming continues without changing it so the existing device preset can be tested; "
                  "frame parsing will report any missing semantic metadata.");
        }
        const TriggerConfigurationSnapshot triggerSnapshot = readTriggerConfigurationSnapshot(device);
        triggerDiagnosticSummary = triggerConfigurationSnapshotSummary(triggerSnapshot);
        logInfo("Acquisition trigger snapshot: " + triggerDiagnosticSummary + ".");
        if (!triggerSnapshot.selectorReadable) {
            if (errorMessage) {
                *errorMessage = "Could not inspect the Heliotis TriggerSelector before acquisition: "
                    + triggerConfigurationSnapshotSummary(triggerSnapshot);
            }
            logWarning("Acquisition arm blocked because TriggerSelector is unavailable.");
            return false;
        }
        if (!triggerSnapshot.restoreSucceeded) {
            if (errorMessage) {
                *errorMessage = "Could not restore the Heliotis TriggerSelector after diagnostic inspection: "
                    + triggerSnapshot.restoreError;
            }
            logWarning("Acquisition arm blocked because trigger selector restoration failed: "
                + triggerSnapshot.restoreError);
            return false;
        }
        const internal::AcquisitionTriggerPlan triggerPlan = internal::evaluateAcquisitionTriggerPlan(
            triggerSnapshot.acquisitionStart,
            triggerSnapshot.frameStart,
            triggerSnapshot.recordingStart);
        if (!triggerPlan.valid) {
            if (errorMessage) *errorMessage = triggerPlan.error;
            logWarning("Acquisition arm blocked by trigger policy: " + triggerPlan.error
                + " snapshot=" + triggerDiagnosticSummary + ".");
            return false;
        }
        if (!triggerPlan.warning.empty()) {
            logWarning("Acquisition trigger plan warning: " + triggerPlan.warning);
        }
        softwareTriggerAvailable = triggerPlan.usesHostSoftwareTrigger();
        softwareTriggered = softwareTriggerAvailable;
        logInfo("Acquisition trigger plan: " + triggerPlan.summary()
            + ", workerMode=" + (softwareTriggered ? std::string("software-wait") : std::string("buffer-poll"))
            + ".");
        if (softwareTriggerAvailable) {
            std::string triggerValidationError;
            if (!validateFrameStartSoftwareTrigger(device, &triggerValidationError)) {
                if (errorMessage) *errorMessage = "TriggerSoftware is unavailable for FrameStart: "
                    + triggerValidationError;
                return false;
            }
        }
        std::string acquisitionModeError;
        if (!readDeviceEnumerationValue(
                device, "AcquisitionMode", &originalAcquisitionMode, &acquisitionModeError)) {
            if (errorMessage) {
                *errorMessage = "Could not read AcquisitionMode before arming: " + acquisitionModeError;
            }
            return false;
        }
        const std::string requestedAcquisitionMode = mode == AcquisitionMode::SingleFrame
            ? "SingleFrame"
            : "Continuous";
        restoreAcquisitionMode = originalAcquisitionMode != requestedAcquisitionMode;
        logInfo("AcquisitionMode policy [arm=" + std::to_string(acquisitionId)
            + "]: original=" + originalAcquisitionMode
            + ", requested=" + requestedAcquisitionMode
            + ", changed=" + (restoreAcquisitionMode ? "true" : "false") + ".");
        if (restoreAcquisitionMode) {
            std::string modeApplyError;
            if (!writeAndVerifyEnumeration(
                    device, "AcquisitionMode", requestedAcquisitionMode, &modeApplyError)) {
                std::string rollbackError;
                if (!writeAndVerifyEnumeration(
                        device, "AcquisitionMode", originalAcquisitionMode, &rollbackError)) {
                    modeApplyError += "; rollback failed: " + rollbackError;
                }
                if (errorMessage) *errorMessage = modeApplyError;
                logWarning("Acquisition arm failed while applying AcquisitionMode="
                    + requestedAcquisitionMode + ": " + modeApplyError);
                return false;
            }
        }
        // h8SurfSimple always starts with four C4Utility-managed receive slots.
        // A single logical capture still needs this transport queue; it stops
        // after the first deep-copied frame.
        constexpr std::int64_t bufferCount = 4;
        std::int64_t payloadSize = 0;
        if (C4Dev_readInteger(device, "PayloadSize", &payloadSize) == C4HDL_ERR_SUCCESS) {
            logInfo("H8 acquisition payload size=" + std::to_string(payloadSize) + " byte(s).");
        } else {
            logInfo("H8 acquisition payload size is not exposed by this device configuration.");
        }
        logInfo("Acquisition device state before start [arm=" + std::to_string(acquisitionId)
            + "]: " + acquisitionDeviceStateSummary(device) + ".");
        logInfo(std::string("Calling C4Dev_startAcquisition [arm=") + std::to_string(acquisitionId)
            + "] with " + std::to_string(bufferCount)
            + " C4Utility receive buffer slot(s). triggerState=" + triggerDiagnosticSummary + ".");
        const auto sdkStartStarted = std::chrono::steady_clock::now();
        const C4HDL_ERROR startResult = C4Dev_startAcquisition(device, bufferCount);
        const auto sdkStartElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - sdkStartStarted);
        if (startResult != C4HDL_ERR_SUCCESS) {
            const std::string startError = operationError("C4Dev_startAcquisition", startResult);
            if (errorMessage) *errorMessage = startError;
            logWarning("C4Dev_startAcquisition failed [arm=" + std::to_string(acquisitionId)
                + ", elapsedMs=" + std::to_string(sdkStartElapsed.count()) + "]: " + startError
                + ". Device state: " + acquisitionDeviceStateSummary(device) + ".");
            if (restoreAcquisitionMode) {
                std::string restoreError;
                if (!writeAndVerifyEnumeration(
                        device, "AcquisitionMode", originalAcquisitionMode, &restoreError)) {
                    logWarning("AcquisitionMode rollback failed after arm failure: " + restoreError);
                    if (errorMessage) *errorMessage += "; AcquisitionMode rollback failed: " + restoreError;
                }
            }
            return false;
        }
        logInfo("C4Dev_startAcquisition succeeded [arm=" + std::to_string(acquisitionId)
            + ", elapsedMs=" + std::to_string(sdkStartElapsed.count())
            + "]. Device state after start: " + acquisitionDeviceStateSummary(device) + ".");

        _stopAcquisitionRequested.store(false);
        {
            std::lock_guard<std::mutex> triggerLock(_triggerMutex);
            _pendingSoftwareTriggers = 0;
            _softwareTriggerInFlight = false;
        }
        _softwareTriggeredAcquisition = softwareTriggered;
        _softwareTriggerAvailable = softwareTriggerAvailable;
        _acquiring = true;
        _acquisitionWorkerInstalling = true;
        _activeAcquisitionId = acquisitionId;
        _lastAcquisitionError.clear();
    }
    try {
        {
            std::lock_guard<std::mutex> startLock(_acquisitionStartMutex);
            _acquisitionWorkerReleased = false;
        }
        std::thread worker(
            &HeliotisC4Device::acquisitionLoop,
            this,
            device,
            mode,
            softwareTriggered,
            std::move(triggerDiagnosticSummary),
            acquisitionId,
            originalAcquisitionMode,
            restoreAcquisitionMode,
            std::move(frameCallback));
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _acquisitionThread = std::move(worker);
            _acquisitionWorkerInstalling = false;
        }
        _acquisitionWorkerCondition.notify_all();
    } catch (const std::exception& exception) {
        C4HDL_ERROR stopResult = C4HDL_ERR_ERROR;
        std::string stopError;
        {
            std::lock_guard<std::mutex> lock(_sdkMutex);
            stopResult = C4Dev_stopAcquisition(device);
            if (stopResult != C4HDL_ERR_SUCCESS) {
                stopError = operationError("C4Dev_stopAcquisition", stopResult);
            }
            if (stopResult == C4HDL_ERR_SUCCESS && restoreAcquisitionMode) {
                std::string restoreError;
                if (!writeAndVerifyEnumeration(
                        device, "AcquisitionMode", originalAcquisitionMode, &restoreError)) {
                    logWarning("AcquisitionMode rollback failed after worker creation failure: " + restoreError);
                }
            } else if (stopResult != C4HDL_ERR_SUCCESS && restoreAcquisitionMode) {
                logWarning("AcquisitionMode rollback was skipped after worker creation failure because SDK stop failed.");
            }
        }
        std::string workerError = exception.what();
        if (stopResult != C4HDL_ERR_SUCCESS) {
            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                _requiresReconnect = true;
            }
            workerError += "; " + stopError + ". Reconnect is required.";
        }
        setAcquisitionError(workerError);
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            _acquiring = false;
            _acquisitionWorkerInstalling = false;
            _softwareTriggeredAcquisition = false;
            _softwareTriggerAvailable = false;
            _activeAcquisitionId = 0;
        }
        _acquisitionWorkerCondition.notify_all();
        if (errorMessage) *errorMessage = workerError;
        return false;
    }

    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _activeStatusDispatching = true;
        _activeStatusDispatchThreadId = std::this_thread::get_id();
    }
    dispatchStatus(Status::Acquisition, true);
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _activeStatusDispatching = false;
        _activeStatusDispatchThreadId = {};
    }
    {
        std::lock_guard<std::mutex> startLock(_acquisitionStartMutex);
        _acquisitionWorkerReleased = true;
    }
    _acquisitionStartCondition.notify_all();
    logInfo(std::string("Acquisition arm dispatch completed [arm=") + std::to_string(acquisitionId)
        + "]: mode=" + modeName
        + ", active=" + (isAcquiring() ? "true" : "false") + ".");
    return true;
}

void HeliotisC4Device::requestStopAcquisition() noexcept
{
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        shouldNotify = _acquiring || _acquisitionThread.joinable();
        if (shouldNotify) _stopAcquisitionRequested.store(true);
    }
    if (!shouldNotify) return;

    {
        std::lock_guard<std::mutex> triggerLock(_triggerMutex);
        _pendingSoftwareTriggers = 0;
    }
    _triggerCondition.notify_all();
}

void HeliotisC4Device::stopAcquisition()
{
    // Only one teardown owner may move/join the worker and publish the final
    // inactive state. Without this fence, two synchronous callers can let the
    // second one clear the stop flag while the first is still joining.
    std::lock_guard<std::mutex> stopLock(_acquisitionStopMutex);
    requestStopAcquisition();

    std::thread worker;
    {
        std::unique_lock<std::mutex> lock(_stateMutex);
        if (_activeStatusDispatching
            && _activeStatusDispatchThreadId == std::this_thread::get_id()) {
            logInfo("Synchronous stop was converted to a non-blocking request inside the active status callback.");
            return;
        }
        _acquisitionWorkerCondition.wait(lock, [this] {
            return !_acquisitionWorkerInstalling;
        });
        if (!_acquisitionThread.joinable() && !_acquiring) return;
        worker = std::move(_acquisitionThread);
    }
    if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _acquisitionThread = std::move(worker);
        return;
    }
    if (worker.joinable()) {
        logInfo("Waiting for the acquisition worker to complete synchronous teardown.");
        worker.join();
    }

    finishAcquisition();
}

bool HeliotisC4Device::isAcquiring() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _acquiring;
}

bool HeliotisC4Device::isSoftwareTriggeredAcquisition() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _acquiring && _softwareTriggeredAcquisition;
}

bool HeliotisC4Device::isSoftwareTriggerAvailable() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _acquiring && _softwareTriggerAvailable;
}

bool HeliotisC4Device::requiresReconnect() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _requiresReconnect;
}

std::string HeliotisC4Device::lastAcquisitionError() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastAcquisitionError;
}

bool HeliotisC4Device::copyFrame(
    const C4_BUFFER buffer,
    const bool detailedDiagnostics,
    Frame* frame,
    std::string* errorMessage) const
{
    if (!buffer || !frame) {
        if (errorMessage) *errorMessage = "C4Utility returned an invalid acquisition buffer.";
        return false;
    }

    std::int64_t partCount = 0;
    if (!checkBufferOperation(C4Buf_getNumParts(buffer, &partCount), "C4Buf_getNumParts", errorMessage) || partCount <= 0) {
        if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility buffer has no data parts.";
        return false;
    }

    std::int64_t chunkPartCount = 0;
    const C4HDL_ERROR chunkPartCountResult = C4Buf_readInteger(buffer, "ChunkPartCount", &chunkPartCount);
    if (chunkPartCountResult == C4HDL_ERR_SUCCESS && chunkPartCount != partCount) {
        if (errorMessage) {
            *errorMessage = "C4Utility buffer ChunkPartCount=" + std::to_string(chunkPartCount)
                + " does not match C4Buf_getNumParts=" + std::to_string(partCount) + ".";
        }
        return false;
    }
    if (detailedDiagnostics) {
        if (chunkPartCountResult == C4HDL_ERR_SUCCESS) {
            logInfo("C4Utility multipart count: C4Buf_getNumParts=" + std::to_string(partCount)
                + ", ChunkPartCount=" + std::to_string(chunkPartCount) + ".");
        } else {
            logWarning("ChunkPartCount metadata is unavailable; using authoritative C4Buf_getNumParts="
                + std::to_string(partCount) + ". Detail: "
                + operationError("C4Buf_readInteger(ChunkPartCount)", chunkPartCountResult) + ".");
        }
    }

    // h8SurfSimple requires only multipart identity chunks.  Geometry chunks
    // are optional: preserve them when the device emits them, but never reject
    // an otherwise valid Range/Reflectance frame because they are absent.
    Scan3dGeometry scan3dGeometry;
    std::string scan3dGeometryError;
    const bool hasScan3dGeometry = readScan3dGeometry(
        buffer, &scan3dGeometry, &scan3dGeometryError);
    if (!hasScan3dGeometry && detailedDiagnostics) {
        logWarning("C4Utility frame has no complete Scan3d geometry metadata: "
            + (scan3dGeometryError.empty()
                ? std::string("the optional geometry chunks are unavailable")
                : scan3dGeometryError)
            + ". Range capture remains valid, but physical grid reconstruction is unavailable.");
    }

    Frame copied;
    std::int64_t chunkFrameId = 0;
    if (C4Buf_readInteger(buffer, "ChunkFrameID", &chunkFrameId) == C4HDL_ERR_SUCCESS && chunkFrameId >= 0) {
        copied.frameId = std::to_string(chunkFrameId);
    }
    std::int64_t chunkTimestamp = 0;
    if (C4Buf_readInteger(buffer, "ChunkTimestamp", &chunkTimestamp) == C4HDL_ERR_SUCCESS && chunkTimestamp >= 0) {
        copied.timestampNs = static_cast<std::uint64_t>(chunkTimestamp);
    }
    copied.parts.reserve(static_cast<std::size_t>(partCount));
    bool hasRangePart = false;
    for (std::int64_t partIndex = 0; partIndex < partCount; ++partIndex) {
        const C4HDL_ERROR partSelectorResult = C4Buf_writeInteger(buffer, "ChunkPartSelector", partIndex);
        if (partSelectorResult != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = operationError("C4Buf_writeInteger(ChunkPartSelector)", partSelectorResult);
            }
            return false;
        }
        std::string metadataError;
        const std::string partName = readSdkString(
            [buffer](char* text, std::size_t* size) {
                return C4Buf_readString(buffer, "ChunkPartType", text, size);
            },
            &metadataError);
        if (!metadataError.empty() || partName.empty()) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer part " + std::to_string(partIndex)
                    + " is missing ChunkPartType metadata"
                    + (metadataError.empty() ? std::string() : ": " + metadataError)
                    + ". Enable ChunkPartType in the device preset/profile; Stage Init intentionally does not change payload settings.";
            }
            return false;
        }

        const FramePartKind partKind = framePartKind(partName);
        if (partKind == FramePartKind::Unknown) {
            if (detailedDiagnostics) {
                logWarning("Skipping unsupported C4Utility chunk part [index="
                    + std::to_string(partIndex) + ", type=" + partName
                    + "]; known Range data in the same frame will still be delivered.");
            }
            continue;
        }

        std::vector<std::int64_t> dimensions;
        if (!readBufferPartDimensions(
                buffer, partIndex, detailedDiagnostics, &dimensions, errorMessage)) return false;

        std::uint64_t expectedSamples = 1;
        for (const std::int64_t dimension : dimensions) {
            if (dimension <= 0
                || dimension > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())
                || expectedSamples > (std::numeric_limits<std::uint32_t>::max)() / static_cast<std::uint64_t>(dimension)) {
                if (errorMessage) *errorMessage = "C4Utility returned unsupported buffer dimensions.";
                return false;
            }
            expectedSamples *= static_cast<std::uint64_t>(dimension);
        }
        if (expectedSamples == 0 || expectedSamples > (std::numeric_limits<std::uint32_t>::max)()) {
            if (errorMessage) *errorMessage = "C4Utility buffer part exceeds the supported sample count.";
            return false;
        }

        const auto width = static_cast<std::uint32_t>(dimensions[0]);
        const auto height = static_cast<std::uint32_t>(expectedSamples / width);

        std::int64_t pixelFormat = 0;
        if (!checkBufferOperation(
                C4Buf_getPartPixelformat(buffer, partIndex, &pixelFormat),
                "C4Buf_getPartPixelformat",
                errorMessage)) return false;
        std::string pixelFormatError;
        const std::string pixelFormatName = readSdkString(
            [buffer, pixelFormat](char* text, std::size_t* size) {
                return C4Buf_getPixelformatName(buffer, pixelFormat, text, size);
            },
            &pixelFormatError);
        if (!pixelFormatError.empty()) {
            if (errorMessage) *errorMessage = pixelFormatError;
            return false;
        }

        FramePart part;
        part.kind = partKind;
        hasRangePart = hasRangePart || part.kind == FramePartKind::Range;
        part.name = partName;
        part.pixelFormat = pixelFormatName;
        part.width = width;
        part.height = height;
        part.bitsPerSample = sourceBitsPerSample(pixelFormatName);
        double fixedPointScale = 1.0;
        bool hasFixedPointScale = false;
        const bool floatingPointSamples = usesFloatingPointSamples(pixelFormatName);
        if (!floatingPointSamples
            && C4Buf_readFloat(buffer, "ChunkPartFixpointScaling", &fixedPointScale) == C4HDL_ERR_SUCCESS) {
            if (!std::isfinite(fixedPointScale)) {
                if (errorMessage) *errorMessage = "C4Utility returned an invalid ChunkPartFixpointScaling value.";
                return false;
            }
            part.sampleScale = fixedPointScale;
            hasFixedPointScale = true;
        }
        const std::string partDiagnosticPrefix = "C4Utility buffer part " + std::to_string(partIndex)
            + " type=" + partName
            + ", pixelFormat=" + pixelFormatName
            + ", reader=" + (floatingPointSamples ? "float" : "uint16")
            + ", ChunkPartFixpointScaling="
            + (hasFixedPointScale ? std::to_string(fixedPointScale) : "unavailable")
            + ", appliedScale=" + std::to_string(part.sampleScale)
            + ", ";
        std::uint32_t sampleCount = static_cast<std::uint32_t>(expectedSamples);
        if (floatingPointSamples) {
            std::vector<double> samples;
            if (!copyBufferPartSamples<double>(
                    sampleCount,
                    "C4Buf_getDataPartFloat",
                    [buffer, partIndex](double* data, std::uint32_t* capacity) {
                        return C4Buf_getDataPartFloat(buffer, partIndex, data, capacity);
                    },
                    &samples,
                    errorMessage)) return false;
            if (detailedDiagnostics) {
                logInfo(partDiagnosticPrefix + "sourceBits="
                    + std::to_string(part.bitsPerSample) + ", " + sampleDiagnostics(samples) + ".");
            }
            part.samples = std::move(samples);
        } else {
            std::vector<std::uint16_t> samples;
            if (!copyBufferPartSamples<std::uint16_t>(
                    sampleCount,
                    "C4Buf_getDataPartUint16",
                    [buffer, partIndex](std::uint16_t* data, std::uint32_t* capacity) {
                        return C4Buf_getDataPartUint16(buffer, partIndex, data, capacity);
                    },
                    &samples,
                    errorMessage)) return false;
            if (detailedDiagnostics) {
                logInfo(partDiagnosticPrefix + "sourceBits="
                    + std::to_string(part.bitsPerSample) + ", " + sampleDiagnostics(samples) + ".");
            }
            part.samples = std::move(samples);
        }
        copied.parts.push_back(std::move(part));
    }

    if (!hasRangePart) {
        if (errorMessage) *errorMessage = "C4Utility frame contains no supported Range/Surface part.";
        return false;
    }
    if (!copied.isValid()) {
        if (errorMessage) *errorMessage = "C4Utility produced an invalid frame payload.";
        return false;
    }
    if (hasScan3dGeometry) {
        copied.scan3dGeometry = std::move(scan3dGeometry);
    }
    *frame = std::move(copied);
    return true;
}

void HeliotisC4Device::acquisitionLoop(
    const C4_DEVICE device,
    const AcquisitionMode mode,
    const bool softwareTriggered,
    std::string triggerDiagnosticSummary,
    const std::uint64_t acquisitionId,
    std::string originalAcquisitionMode,
    const bool restoreAcquisitionMode,
    FrameCallback frameCallback)
{
    {
        std::unique_lock<std::mutex> startLock(_acquisitionStartMutex);
        _acquisitionStartCondition.wait(startLock, [this] {
            return _acquisitionWorkerReleased;
        });
    }
    // C4Dev_getBuffer takes milliseconds.  Keep the logical acquisition wait
    // at ten seconds, but poll in short slices so stopAcquisition() can acquire
    // _sdkMutex between SDK calls instead of waiting for the full timeout.
    constexpr auto bufferTimeout = std::chrono::seconds(10);
    constexpr auto bufferPollTimeout = std::chrono::milliseconds(100);
    std::size_t timeoutCount = 0;
    std::size_t receivedFrameCount = 0;
    logInfo(std::string("Acquisition worker started [arm=") + std::to_string(acquisitionId)
        + "]: mode="
        + (mode == AcquisitionMode::SingleFrame ? "Single" : "Live")
        + (softwareTriggered ? ", FrameStart source=Software." : ", device trigger configuration preserved.")
        + " triggerState=" + triggerDiagnosticSummary + ".");
    while (!_stopAcquisitionRequested.load()) {
        bool executeSoftwareTrigger = softwareTriggered;
        if (softwareTriggered) {
            std::unique_lock<std::mutex> triggerLock(_triggerMutex);
            _triggerCondition.wait(triggerLock, [this] {
                return _stopAcquisitionRequested.load() || _pendingSoftwareTriggers != 0;
            });
            if (_stopAcquisitionRequested.load()) break;
            --_pendingSoftwareTriggers;
            _softwareTriggerInFlight = true;
        }

        C4_BUFFER buffer = nullptr;
        Frame frame;
        std::string error;
        C4HDL_ERROR result = C4HDL_ERR_ERROR;
        std::chrono::milliseconds bufferWait{};
        std::chrono::milliseconds copyElapsed{};
        auto logicalWaitStarted = std::chrono::steady_clock::now();
        bool triggerIssued = !executeSoftwareTrigger;
        while (!_stopAcquisitionRequested.load()) {
            const auto pollStarted = std::chrono::steady_clock::now();
            error.clear();
            result = C4HDL_ERR_SUCCESS;
            C4HDL_ERROR getBufferResult = C4HDL_ERR_SUCCESS;
            bool getBufferFailed = false;
            {
                std::lock_guard<std::mutex> lock(_sdkMutex);
                if (_stopAcquisitionRequested.load()) {
                    result = C4HDL_ERR_ERROR;
                    error = "Acquisition stop requested before the next C4Utility operation.";
                } else if (!triggerIssued) {
                    logInfo("Acquisition worker executing queued FrameStart TriggerSoftware [arm="
                        + std::to_string(acquisitionId) + "].");
                    const bool triggered = executeFrameStartSoftwareTrigger(device, &error);
                    triggerIssued = true;
                    if (!triggered) result = C4HDL_ERR_ERROR;
                }
                if (result == C4HDL_ERR_SUCCESS) {
                    getBufferResult = C4Dev_getBuffer(device, &buffer, bufferPollTimeout.count());
                    result = getBufferResult;
                    getBufferFailed = getBufferResult != C4HDL_ERR_SUCCESS;
                    if (getBufferFailed) {
                        error = operationError("C4Dev_getBuffer", getBufferResult);
                    }
                }
                if (result == C4HDL_ERR_SUCCESS) {
                    bool copied = false;
                    const bool detailedDiagnostics = mode == AcquisitionMode::SingleFrame
                        || receivedFrameCount < 3
                        || (receivedFrameCount + 1) % 100 == 0;
                    const auto copyStarted = std::chrono::steady_clock::now();
                    try {
                        copied = copyFrame(buffer, detailedDiagnostics, &frame, &error);
                    } catch (const std::exception& exception) {
                        error = exception.what();
                    } catch (...) {
                        error = "An unknown exception occurred while copying a C4Utility buffer.";
                    }
                    copyElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - copyStarted);
                    const C4HDL_ERROR releaseResult = C4Buf_release(buffer);
                    buffer = nullptr;
                    if (!copied && error.empty()) error = "C4Utility could not copy an acquisition buffer.";
                    if (releaseResult != C4HDL_ERR_SUCCESS && error.empty()) {
                        error = operationError("C4Buf_release", releaseResult);
                    }
                    if (!error.empty()) result = C4HDL_ERR_ERROR;
                } else if (error.empty()) {
                    error = operationError("C4Utility acquisition", result);
                }
            }

            bufferWait = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - logicalWaitStarted);
            if (result == C4HDL_ERR_SUCCESS) break;
            if (_stopAcquisitionRequested.load()) break;

            const auto pollWait = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - pollStarted);
            // Only a failed getBuffer wait may be treated as a timeout. Copy,
            // metadata, release, and software-command failures must terminate
            // with their real error even when they took longer than one poll.
            if (getBufferFailed
                && isLikelyTimeoutError(error, getBufferResult, pollWait, bufferPollTimeout)) {
                if (bufferWait >= bufferTimeout) {
                    ++timeoutCount;
                    if (timeoutCount <= 3 || timeoutCount % 20 == 0) {
                        std::string deviceState;
                        std::string currentTriggerState;
                        {
                            std::lock_guard<std::mutex> lock(_sdkMutex);
                            deviceState = acquisitionDeviceStateSummary(device);
                            currentTriggerState = triggerConfigurationSnapshotSummary(
                                readTriggerConfigurationSnapshot(device));
                        }
                        logInfo("Acquisition is armed but waiting for a frame/trigger [arm="
                            + std::to_string(acquisitionId)
                            + ", timeouts=" + std::to_string(timeoutCount)
                            + ", logicalWaitMs=" + std::to_string(bufferWait.count())
                            + ", lastPollError=" + error
                            + ", armTriggerState=" + triggerDiagnosticSummary
                            + ", currentTriggerState=" + currentTriggerState
                            + ", deviceState=" + deviceState + "].");
                    }
                    logicalWaitStarted = std::chrono::steady_clock::now();
                }
                continue;
            }
            break;
        }

        // Continuous acquisition may accept the next command as soon as its
        // frame has been copied. SingleFrame keeps the command in flight until
        // disarm so a fast caller cannot queue a second command that the worker
        // would necessarily discard after delivering its one frame.
        if (executeSoftwareTrigger && mode == AcquisitionMode::Continuous) {
            std::lock_guard<std::mutex> triggerLock(_triggerMutex);
            _softwareTriggerInFlight = false;
        }

        if (result != C4HDL_ERR_SUCCESS) {
            if (_stopAcquisitionRequested.load()) break;
            std::string failureDeviceState;
            std::string currentTriggerState;
            {
                std::lock_guard<std::mutex> lock(_sdkMutex);
                failureDeviceState = acquisitionDeviceStateSummary(device);
                currentTriggerState = triggerConfigurationSnapshotSummary(
                    readTriggerConfigurationSnapshot(device));
            }
            logWarning("Acquisition buffer wait failed [arm=" + std::to_string(acquisitionId) + "]: "
                + (error.empty() ? std::string("C4Utility acquisition failed.") : error)
                + ". armTriggerState=" + triggerDiagnosticSummary
                + ", currentTriggerState=" + currentTriggerState
                + ", deviceState=" + failureDeviceState + ".");
            setAcquisitionError(error.empty() ? "C4Utility acquisition failed." : error);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            frame.sequence = _nextFrameSequence++;
        }
        ++receivedFrameCount;
        if (mode == AcquisitionMode::SingleFrame || receivedFrameCount <= 3 || receivedFrameCount % 100 == 0) {
            logInfo("Received frame [arm=" + std::to_string(acquisitionId)
                + ", sequence=" + std::to_string(frame.sequence)
                + ", parts=" + std::to_string(frame.parts.size()) + ", count="
                + std::to_string(receivedFrameCount)
                + ", waitMs=" + std::to_string(bufferWait.count())
                + ", copyMs=" + std::to_string(copyElapsed.count()) + "].");
        }
        const auto callbackStarted = std::chrono::steady_clock::now();
        try {
            frameCallback(std::move(frame));
        } catch (const std::exception& exception) {
            setAcquisitionError(exception.what());
            break;
        } catch (...) {
            setAcquisitionError("An unknown exception occurred in the Heliotis frame callback.");
            break;
        }
        const auto callbackElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - callbackStarted);
        if (mode == AcquisitionMode::SingleFrame || receivedFrameCount <= 3
            || receivedFrameCount % 100 == 0 || callbackElapsed >= std::chrono::milliseconds(100)) {
            logInfo("Frame callback completed [arm=" + std::to_string(acquisitionId)
                + ", count=" + std::to_string(receivedFrameCount)
                + ", elapsedMs=" + std::to_string(callbackElapsed.count()) + "].");
        }
        if (mode == AcquisitionMode::SingleFrame) break;
    }

    const bool stopRequested = _stopAcquisitionRequested.load();
    C4HDL_ERROR stopResult = C4HDL_ERR_ERROR;
    std::string sdkStopError;
    std::string acquisitionModeRestoreError;
    std::string stateAfterStop;
    const auto sdkStopStarted = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(_sdkMutex);
        stopResult = C4Dev_stopAcquisition(device);
        if (stopResult != C4HDL_ERR_SUCCESS) {
            sdkStopError = operationError("C4Dev_stopAcquisition", stopResult);
            stateAfterStop = "<not queried after failed SDK stop>";
            if (restoreAcquisitionMode) {
                logWarning("AcquisitionMode restore was skipped [arm="
                    + std::to_string(acquisitionId) + "] because SDK stop failed.");
            }
        } else {
            if (restoreAcquisitionMode
                && !writeAndVerifyEnumeration(
                    device, "AcquisitionMode", originalAcquisitionMode, &acquisitionModeRestoreError)) {
                logWarning("AcquisitionMode restore failed [arm=" + std::to_string(acquisitionId)
                    + "]: " + acquisitionModeRestoreError);
            }
            stateAfterStop = acquisitionDeviceStateSummary(device);
        }
    }
    const auto sdkStopElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sdkStopStarted);
    if (stopRequested) {
        logInfo("Acquisition worker observed a non-blocking stop request [arm="
            + std::to_string(acquisitionId) + "].");
    }
    if (stopResult == C4HDL_ERR_SUCCESS) {
        logInfo("Acquisition worker completed C4Dev_stopAcquisition [arm="
            + std::to_string(acquisitionId)
            + ", elapsedMs=" + std::to_string(sdkStopElapsed.count())
            + ", restoredAcquisitionMode="
            + (!restoreAcquisitionMode
                ? "not-needed"
                : (acquisitionModeRestoreError.empty() ? "true" : "false"))
            + ", state=" + stateAfterStop + "].");
    } else {
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _requiresReconnect = true;
        }
        setAcquisitionError("Acquisition stop failed [arm=" + std::to_string(acquisitionId)
            + "]: " + sdkStopError + ". Disconnect and reconnect before re-arming.");
        logWarning("Acquisition worker C4Dev_stopAcquisition failed [arm="
            + std::to_string(acquisitionId) + ", elapsedMs="
            + std::to_string(sdkStopElapsed.count()) + "]: " + sdkStopError);
    }
    if (!acquisitionModeRestoreError.empty() && stopResult == C4HDL_ERR_SUCCESS) {
        setAcquisitionError("AcquisitionMode restore failed [arm=" + std::to_string(acquisitionId)
            + "]: " + acquisitionModeRestoreError);
    }
    logInfo("Acquisition worker finished [arm=" + std::to_string(acquisitionId)
        + "]: frames=" + std::to_string(receivedFrameCount)
        + ", stopRequested=" + (stopRequested ? "true" : "false")
        + ", triggerState=" + triggerDiagnosticSummary + ".");
    finishAcquisition();
}

void HeliotisC4Device::finishAcquisition()
{
    bool wasAcquiring = false;
    std::uint64_t acquisitionId = 0;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        wasAcquiring = _acquiring;
        acquisitionId = _activeAcquisitionId;
        _acquiring = false;
        _acquisitionWorkerInstalling = false;
        _softwareTriggeredAcquisition = false;
        _softwareTriggerAvailable = false;
        _activeAcquisitionId = 0;
    }
    _acquisitionWorkerCondition.notify_all();
    {
        std::lock_guard<std::mutex> triggerLock(_triggerMutex);
        _pendingSoftwareTriggers = 0;
        _softwareTriggerInFlight = false;
    }
    _stopAcquisitionRequested.store(false);
    if (wasAcquiring) dispatchStatus(Status::Acquisition, false);
    if (wasAcquiring) {
        logInfo("Acquisition disarmed [arm=" + std::to_string(acquisitionId) + "].");
    }
}

void HeliotisC4Device::setAcquisitionError(std::string message)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastAcquisitionError = std::move(message);
    logWarning("Acquisition error: " + _lastAcquisitionError);
}

HeliotisC4Device::CallbackId HeliotisC4Device::registerStatusCallback(StatusCallback callback)
{
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(_stateMutex);
    const CallbackId id = _nextCallbackId++;
    _statusCallbacks.emplace(id, std::move(callback));
    return id;
}

bool HeliotisC4Device::deregisterStatusCallback(const CallbackId id)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _statusCallbacks.erase(id) != 0;
}

void HeliotisC4Device::dispatchConnectionStatus(const bool connected)
{
    dispatchStatus(Status::Connection, connected);
}

void HeliotisC4Device::dispatchStatus(const Status status, const bool active)
{
    std::vector<StatusCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        callbacks.reserve(_statusCallbacks.size());
        for (const auto& [id, callback] : _statusCallbacks) callbacks.push_back(callback);
    }
    for (std::size_t index = 0; index < callbacks.size(); ++index) {
        try {
            callbacks[index](status, active);
        } catch (const std::exception& exception) {
            logWarning("Status callback " + std::to_string(index)
                + " threw an exception: " + exception.what());
        } catch (...) {
            logWarning("Status callback " + std::to_string(index)
                + " threw an unknown exception.");
        }
    }
}

} // namespace heliotis
