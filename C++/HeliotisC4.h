#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace heliotis {

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

enum class FeatureSection {
    Device,
    Motion
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

struct FramePart {
    FramePartKind kind = FramePartKind::Unknown;
    std::string name;
    std::string pixelFormat;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitsPerSample = 0;
    std::variant<std::vector<std::uint16_t>, std::vector<double>> samples;

    [[nodiscard]] bool isValid() const noexcept
    {
        if (width == 0 || height == 0)
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
        return std::visit([expectedSampleCount](const auto& values) {
            return values.size() == expectedSampleCount;
        }, samples);
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
    FeatureSection section = FeatureSection::Device;
    std::string categoryPath;
    std::string displayName;
    std::string valueText;
    std::string description;
    FeatureType type = FeatureType::Unknown;
    FeatureAccess access = FeatureAccess::Unknown;
};

class HeliotisC4 {
public:
    using FeatureList = std::vector<FeatureDescriptor>;
};

} // namespace heliotis
