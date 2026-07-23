#pragma once

#include <string>
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

// A deep-owned, SDK-neutral view of C4 feature metadata for the Qt control tree.
struct FeatureDescriptor {
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
