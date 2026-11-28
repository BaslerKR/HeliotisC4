#pragma once

#include "HeliotisFrameAnomalies.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace heliotis {

/** @brief Per-run producer diagnostics; waiting for a trigger is not a failed frame. */
struct RuntimeDiagnostics {
    bool grabbing = false;
    std::uint64_t delivered = 0;
    std::uint64_t failed = 0;
};

enum class FeatureType {
    Unknown,
    Integer,
    Float,
    String,
    Enumeration,
    Command,
    Boolean
};

enum class FeatureAccess {
    Unknown,
    NotImplemented,
    NotAvailable,
    ReadOnly,
    WriteOnly,
    ReadWrite
};

/** Selects host-side Single or Live completion; device acquisition stays Continuous. */
enum class AcquisitionMode {
    SingleFrame,
    Continuous
};

struct DeviceDescriptor {
    std::int64_t interfaceIndex = -1;
    std::int64_t deviceIndex = -1;
    std::string interfaceName;
    std::string deviceName;
};

// SDK-neutral, deep-owned payload types. C4 buffers must be copied into this
// contract before C4Buf_release(), so hosts can consume a frame independently
// of the C4Utility lifetime.
enum class FramePartKind {
    Unknown,
    Range,
    Intensity,
    Confidence
};

/**
 * One copied buffer part.
 *
 * `width` and `height` are the declared shape. `samples` holds exactly what the
 * SDK delivered, so a short or longer payload is kept as delivered and the
 * consumer fits it to the declared shape (zero-filling a short tail). The
 * `std::uint8_t` alternative carries bytes whose length is not a whole number of
 * samples, or whose format is unknown; the consumer publishes them as raw Mono8.
 */
struct FramePart {
    FramePartKind kind = FramePartKind::Unknown;
    std::string name;
    std::string pixelFormat;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitsPerSample = 0;
    std::variant<std::vector<std::uint16_t>, std::vector<double>, std::vector<std::uint8_t>> samples;
    double sampleScale = 1.0;

    /** Samples (or raw bytes) the part carries, independent of its declared shape. */
    [[nodiscard]] std::size_t sampleCount() const noexcept
    {
        return std::visit([](const auto& values) { return values.size(); }, samples);
    }

    /** Bytes the part carries, independent of its declared shape. */
    [[nodiscard]] std::uint64_t sampleBytes() const noexcept
    {
        return std::visit([](const auto& values) {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            return static_cast<std::uint64_t>(values.size()) * sizeof(Value);
        }, samples);
    }

    /** True when the payload is exactly the declared shape; a shape mismatch is still deliverable. */
    [[nodiscard]] bool isValid() const noexcept
    {
        if (width == 0 || height == 0 || !std::isfinite(sampleScale))
        {
            return false;
        }

        const auto unsignedWidth = static_cast<std::size_t>(width);
        const auto unsignedHeight = static_cast<std::size_t>(height);
        if (unsignedHeight > (std::numeric_limits<std::size_t>::max)() / unsignedWidth)
        {
            return false;
        }

        const std::size_t expectedSampleCount = unsignedWidth * unsignedHeight;
        return sampleCount() == expectedSampleCount;
    }
};

// Deep-owned Scan3d chunk metadata for an H8 Surface part.  The values use
// the unit named by distanceUnit and transform a sample as value * scale +
// offset.  RectifiedC supplies a uniform A/B grid; CalibratedC does not.
struct Scan3dGeometry {
    std::string outputMode;
    std::string distanceUnit;
    double xScale = std::numeric_limits<double>::quiet_NaN();
    double yScale = std::numeric_limits<double>::quiet_NaN();
    double zScale = std::numeric_limits<double>::quiet_NaN();
    double xOffset = std::numeric_limits<double>::quiet_NaN();
    double yOffset = std::numeric_limits<double>::quiet_NaN();
    double zOffset = std::numeric_limits<double>::quiet_NaN();
};

struct Frame {
    std::uint64_t sequence = 0;
    std::string frameId;
    std::vector<FramePart> parts;
    std::optional<Scan3dGeometry> scan3dGeometry;
    std::optional<std::uint64_t> timestampNs;
    /** Deviations seen while copying the SDK buffer; the frame was fitted, not rejected. */
    HeliotisFrameAnomalies anomalies;

    /** True when every part is exactly its declared shape; an incomplete frame is still deliverable. */
    [[nodiscard]] bool isValid() const noexcept
    {
        if (parts.empty())
        {
            return false;
        }

        for (const auto& part : parts)
        {
            if (!part.isValid())
            {
                return false;
            }
        }
        return true;
    }
};

// A deep-owned, SDK-neutral view of C4 feature metadata for the Qt control tree.
struct FeatureDescriptor {
    std::string categoryPath;
    std::string displayName;
    std::string valueText;
    std::string description;
    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
    std::vector<std::string> enumEntries;
    // Optional SDK-reported numeric bounds. Absent values were not reported.
    std::optional<std::int64_t> integerMinimum;
    std::optional<std::int64_t> integerMaximum;
    std::optional<std::int64_t> integerIncrement;
    std::optional<double> floatMinimum;
    std::optional<double> floatMaximum;
    std::optional<double> floatIncrement;
};

class HeliotisC4 {
public:
    using FeatureList = std::vector<FeatureDescriptor>;
};

} // namespace heliotis
