#include "HeliotisC4System.h"
#include "Internal/C4UtilitySdkRuntime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace heliotis {

namespace {

std::string lastSdkError()
{
    const char* error = C4Hdl_getLastError();
    return error ? std::string(error) : std::string("Unknown C4Utility error.");
}

bool check(const C4HDL_ERROR result, std::string* errorMessage)
{
    if (result == C4HDL_ERR_SUCCESS) return true;
    if (errorMessage) *errorMessage = lastSdkError();
    return false;
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

bool usesFloatingPointSamples(const FramePartKind kind)
{
    return kind == FramePartKind::Range;
}

bool isLikelyTimeoutError(
    const std::string& message,
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
    const bool genericError = value.empty()
        || value == "unknown c4utility error."
        || value == "error";
    constexpr auto tolerance = std::chrono::milliseconds(25);
    return genericError && elapsed + tolerance >= requestedTimeout;
}

bool validateChunkMetadataConfiguration(const C4_DEVICE device, std::string* errorMessage)
{
    std::int64_t chunkModeActive = 0;
    if (C4Dev_readInteger(device, "ChunkModeActive", &chunkModeActive) != C4HDL_ERR_SUCCESS
        || chunkModeActive != 1) {
        if (errorMessage) {
            *errorMessage = "Heliotis acquisition requires the existing device configuration to enable "
                "ChunkModeActive, ChunkPartCount, ChunkPartType, ChunkScan3dDistanceUnit, "
                "ChunkScan3dOutputMode, ChunkScan3dCoordinateScale, and ChunkScan3dCoordinateOffset. "
                "The plugin does not change device features.";
        }
        return false;
    }
    return true;
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
    const std::array<AxisValues, 3> axes{{
        {"CoordinateA", &copied.xScale, &copied.xOffset},
        {"CoordinateB", &copied.yScale, &copied.yOffset},
        {"CoordinateC", &copied.zScale, &copied.zOffset},
    }};
    for (const auto& axis : axes) {
        if (C4Buf_writeString(buffer, "ChunkScan3dCoordinateSelector", axis.selector) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateScale", axis.scale) != C4HDL_ERR_SUCCESS
            || C4Buf_readFloat(buffer, "ChunkScan3dCoordinateOffset", axis.offset) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer is missing Scan3d chunk geometry for "
                    + std::string(axis.selector) + ".";
            }
            return false;
        }
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

    std::string error;
    if (!check(C4Hdl_open(&_handler), &error)) {
        throw std::runtime_error(error);
    }
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
    if (!check(C4Hdl_updateInterfaceList(_handler, &interfaceCount), errorMessage)) return devices;

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
        }
        C4If_release(interfaceHandle);
    }
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
    close();
    if (!_system) {
        if (errorMessage) *errorMessage = "C4Utility system is unavailable.";
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
        if (!check(C4Hdl_openInterface(_system->_handler, &interfaceHandle, descriptor.interfaceIndex), errorMessage)) return false;
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
    }
    dispatchConnectionStatus(true);
    return true;
}

void HeliotisC4Device::close()
{
    stopAcquisition();

    C4_INTERFACE interfaceHandle = nullptr;
    C4_DEVICE deviceHandle = nullptr;
    bool wasOpen = false;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        wasOpen = _device != nullptr;
        interfaceHandle = _interface;
        deviceHandle = _device;
        _interface = nullptr;
        _device = nullptr;
        _connectedDeviceName.clear();
    }
    if (deviceHandle) C4Dev_release(deviceHandle);
    if (interfaceHandle) C4If_release(interfaceHandle);
    if (wasOpen) dispatchConnectionStatus(false);
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

HeliotisC4::FeatureList HeliotisC4Device::readFeatures(std::string* errorMessage) const
{
    std::scoped_lock lock(_stateMutex, _sdkMutex);
    HeliotisC4::FeatureList features;
    if (!_device) {
        if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
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
        if ((access != FeatureAccess::ReadOnly && access != FeatureAccess::ReadWrite)
            && type != FeatureType::Command) {
            continue;
        }
        features.push_back({
            isMotionFeature(category, name) ? FeatureSection::Motion : FeatureSection::Device,
            category,
            name,
            type == FeatureType::Command ? std::string("Command") : readFeatureValue(feature, type),
            description,
            type,
            access,
        });
    }
    C4Ftr_release(featureList);
    return features;
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

    stopAcquisition();

    C4_DEVICE device = nullptr;
    {
        std::scoped_lock lock(_stateMutex, _sdkMutex);
        if (!_device) {
            if (errorMessage) *errorMessage = "Heliotis C4 device is not connected.";
            return false;
        }

        device = _device;
        if (!validateChunkMetadataConfiguration(device, errorMessage)) return false;
        const std::int64_t bufferCount = mode == AcquisitionMode::SingleFrame ? 1 : 4;
        if (!check(C4Dev_startAcquisition(device, bufferCount), errorMessage)) return false;

        _stopAcquisitionRequested.store(false);
        _acquiring = true;
        _lastAcquisitionError.clear();
    }
    dispatchStatus(Status::Acquisition, true);

    try {
        std::thread worker(&HeliotisC4Device::acquisitionLoop, this, device, mode, std::move(frameCallback));
        std::lock_guard<std::mutex> lock(_stateMutex);
        _acquisitionThread = std::move(worker);
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(_sdkMutex);
            C4Dev_stopAcquisition(device);
        }
        setAcquisitionError(exception.what());
        finishAcquisition();
        if (errorMessage) *errorMessage = exception.what();
        return false;
    }

    return true;
}

void HeliotisC4Device::stopAcquisition()
{
    std::thread worker;
    C4_DEVICE device = nullptr;
    bool wasAcquiring = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_acquisitionThread.joinable() && !_acquiring) return;
        _stopAcquisitionRequested.store(true);
        device = _device;
        wasAcquiring = _acquiring;
        worker = std::move(_acquisitionThread);
    }

    if (wasAcquiring && device) {
        std::lock_guard<std::mutex> lock(_sdkMutex);
        C4Dev_stopAcquisition(device);
    }
    if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _acquisitionThread = std::move(worker);
        return;
    }
    if (worker.joinable()) worker.join();

    finishAcquisition();
}

bool HeliotisC4Device::isAcquiring() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _acquiring;
}

std::string HeliotisC4Device::lastAcquisitionError() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastAcquisitionError;
}

bool HeliotisC4Device::copyFrame(const C4_BUFFER buffer, Frame* frame, std::string* errorMessage) const
{
    if (!buffer || !frame) {
        if (errorMessage) *errorMessage = "C4Utility returned an invalid acquisition buffer.";
        return false;
    }

    std::int64_t partCount = 0;
    if (!check(C4Buf_getNumParts(buffer, &partCount), errorMessage) || partCount <= 0) {
        if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility buffer has no data parts.";
        return false;
    }

    std::int64_t chunkPartCount = 0;
    if (C4Buf_readInteger(buffer, "ChunkPartCount", &chunkPartCount) != C4HDL_ERR_SUCCESS
        || chunkPartCount != partCount) {
        if (errorMessage) {
            *errorMessage = "C4Utility buffer is missing complete ChunkPartCount metadata. "
                "Enable ChunkPartCount and ChunkPartType in the H8 configuration before acquisition.";
        }
        return false;
    }

    Scan3dGeometry scan3dGeometry;
    if (!readScan3dGeometry(buffer, &scan3dGeometry, errorMessage)) return false;

    Frame copied;
    copied.parts.reserve(static_cast<std::size_t>(partCount));
    for (std::int64_t partIndex = 0; partIndex < partCount; ++partIndex) {
        std::array<std::int64_t, 2> dimensions{};
        std::uint32_t dimensionCount = static_cast<std::uint32_t>(dimensions.size());
        if (!check(C4Buf_getPartDimension(buffer, partIndex, dimensions.data(), &dimensionCount), errorMessage)
            || dimensionCount != dimensions.size()
            || dimensions[0] <= 0 || dimensions[1] <= 0
            || dimensions[0] > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())
            || dimensions[1] > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned unsupported buffer dimensions.";
            return false;
        }

        const auto width = static_cast<std::uint32_t>(dimensions[0]);
        const auto height = static_cast<std::uint32_t>(dimensions[1]);
        const auto expectedSamples = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        if (expectedSamples == 0 || expectedSamples > (std::numeric_limits<std::uint32_t>::max)()) {
            if (errorMessage) *errorMessage = "C4Utility buffer part exceeds the supported sample count.";
            return false;
        }

        std::int64_t pixelFormat = 0;
        if (!check(C4Buf_getPartPixelformat(buffer, partIndex, &pixelFormat), errorMessage)) return false;
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

        if (C4Buf_writeInteger(buffer, "ChunkPartSelector", partIndex) != C4HDL_ERR_SUCCESS) {
            if (errorMessage) {
                *errorMessage = "C4Utility buffer cannot select ChunkPartSelector. "
                    "Enable ChunkPartType in the H8 configuration before acquisition.";
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
                *errorMessage = "C4Utility buffer is missing ChunkPartType metadata. "
                    "Enable ChunkPartType in the H8 configuration before acquisition.";
            }
            return false;
        }

        FramePart part;
        part.kind = framePartKind(partName);
        if (part.kind == FramePartKind::Unknown) {
            if (errorMessage) *errorMessage = "C4Utility returned an unsupported H8 chunk part type: " + partName;
            return false;
        }
        part.name = partName;
        part.pixelFormat = pixelFormatName;
        part.width = width;
        part.height = height;
        std::uint32_t sampleCount = static_cast<std::uint32_t>(expectedSamples);
        if (usesFloatingPointSamples(part.kind)) {
            std::vector<double> samples(sampleCount);
            if (!check(C4Buf_getDataPartFloat(buffer, partIndex, samples.data(), &sampleCount), errorMessage)
                || sampleCount != expectedSamples) {
                if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned an invalid floating-point part size.";
                return false;
            }
            part.bitsPerSample = 64;
            part.samples = std::move(samples);
        } else {
            std::vector<std::uint16_t> samples(sampleCount);
            if (!check(C4Buf_getDataPartUint16(buffer, partIndex, samples.data(), &sampleCount), errorMessage)
                || sampleCount != expectedSamples) {
                if (errorMessage && errorMessage->empty()) *errorMessage = "C4Utility returned an invalid uint16 part size.";
                return false;
            }
            part.bitsPerSample = 16;
            part.samples = std::move(samples);
        }
        copied.parts.push_back(std::move(part));
    }

    if (!copied.isValid()) {
        if (errorMessage) *errorMessage = "C4Utility produced an invalid frame payload.";
        return false;
    }
    copied.scan3dGeometry = std::move(scan3dGeometry);
    *frame = std::move(copied);
    return true;
}

void HeliotisC4Device::acquisitionLoop(
    const C4_DEVICE device,
    const AcquisitionMode mode,
    FrameCallback frameCallback)
{
    constexpr auto bufferTimeout = std::chrono::milliseconds(250);
    while (!_stopAcquisitionRequested.load()) {
        C4_BUFFER buffer = nullptr;
        Frame frame;
        std::string error;
        C4HDL_ERROR result = C4HDL_ERR_ERROR;
        std::chrono::milliseconds bufferWait{};
        {
            std::lock_guard<std::mutex> lock(_sdkMutex);
            const auto waitStarted = std::chrono::steady_clock::now();
            result = C4Dev_getBuffer(device, &buffer, bufferTimeout.count());
            bufferWait = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStarted);
            if (result == C4HDL_ERR_SUCCESS) {
                bool copied = false;
                try {
                    copied = copyFrame(buffer, &frame, &error);
                } catch (const std::exception& exception) {
                    error = exception.what();
                } catch (...) {
                    error = "An unknown exception occurred while copying a C4Utility buffer.";
                }
                const C4HDL_ERROR releaseResult = C4Buf_release(buffer);
                buffer = nullptr;
                if (!copied && error.empty()) error = "C4Utility could not copy an acquisition buffer.";
                if (releaseResult != C4HDL_ERR_SUCCESS && error.empty()) error = lastSdkError();
                if (!error.empty()) result = C4HDL_ERR_ERROR;
            } else {
                error = lastSdkError();
            }
        }

        if (result != C4HDL_ERR_SUCCESS) {
            if (_stopAcquisitionRequested.load()) break;
            if (isLikelyTimeoutError(error, bufferWait, bufferTimeout)) continue;
            setAcquisitionError(error.empty() ? "C4Utility acquisition failed." : error);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            frame.sequence = _nextFrameSequence++;
        }
        try {
            frameCallback(std::move(frame));
        } catch (const std::exception& exception) {
            setAcquisitionError(exception.what());
            break;
        } catch (...) {
            setAcquisitionError("An unknown exception occurred in the Heliotis frame callback.");
            break;
        }
        if (mode == AcquisitionMode::SingleFrame) break;
    }

    if (!_stopAcquisitionRequested.load()) {
        std::lock_guard<std::mutex> lock(_sdkMutex);
        C4Dev_stopAcquisition(device);
    }
    finishAcquisition();
}

void HeliotisC4Device::finishAcquisition()
{
    bool wasAcquiring = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        wasAcquiring = _acquiring;
        _acquiring = false;
    }
    if (wasAcquiring) dispatchStatus(Status::Acquisition, false);
}

void HeliotisC4Device::setAcquisitionError(std::string message)
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    _lastAcquisitionError = std::move(message);
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
    for (const auto& callback : callbacks) callback(status, active);
}

} // namespace heliotis
