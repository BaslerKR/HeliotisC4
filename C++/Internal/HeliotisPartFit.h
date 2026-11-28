#pragma once

/**
 * @file HeliotisPartFit.h
 * @brief SDK-free rules that fit one copied buffer part into a deliverable shape.
 *
 * The SDK reports a declared shape (dimensions) and then delivers some number of
 * bytes. Playground never rejects device data because those two disagree: the
 * part keeps its declared shape when it has one and the consumer zero-fills a short
 * tail, or the delivered samples become a one-row preview when no shape was declared.
 * These helpers hold only that decision so it can be tested without the C4Utility SDK.
 */

#include "HeliotisC4.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace heliotis::partfit {

/** How the delivered sample count relates to the declared shape. */
enum class ShapeOutcome {
    /** Delivered exactly the declared sample count. */
    Exact,
    /** Fewer samples than declared; the consumer zero-fills the tail. */
    Truncated,
    /** More samples than declared; the consumer uses the declared shape and ignores the excess. */
    Excess,
    /** No usable declared shape; the delivered samples form one row. */
    UnusableDimensions
};

struct Shape {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ShapeOutcome outcome = ShapeOutcome::Exact;
};

/**
 * Chooses the part shape from the declared dimensions and the delivered sample count.
 *
 * @param dimensionsUsable True when every declared dimension is positive and the product fits 32 bits.
 * @param firstDimension Declared width; ignored without usable dimensions.
 * @param declaredSamples Product of the declared dimensions; ignored without usable dimensions.
 * @param deliveredSamples Samples the SDK actually delivered; zero is treated as one row of width 1.
 */
[[nodiscard]] inline Shape fitShape(const bool dimensionsUsable,
                                    const std::uint64_t firstDimension,
                                    const std::uint64_t declaredSamples,
                                    const std::uint64_t deliveredSamples) noexcept
{
    constexpr std::uint64_t limit = (std::numeric_limits<std::uint32_t>::max)();
    Shape shape;
    if (dimensionsUsable && firstDimension != 0U && firstDimension <= limit
        && declaredSamples != 0U && declaredSamples % firstDimension == 0U) {
        shape.width = static_cast<std::uint32_t>(firstDimension);
        shape.height = static_cast<std::uint32_t>((std::min)(declaredSamples / firstDimension, limit));
        shape.outcome = deliveredSamples == declaredSamples ? ShapeOutcome::Exact
            : deliveredSamples < declaredSamples ? ShapeOutcome::Truncated : ShapeOutcome::Excess;
        return shape;
    }
    shape.width = static_cast<std::uint32_t>((std::min)((std::max)(deliveredSamples, std::uint64_t{1}), limit));
    shape.height = 1U;
    shape.outcome = ShapeOutcome::UnusableDimensions;
    return shape;
}

/** Maps a shape outcome to the anomaly reason it is counted under; Exact has none. */
[[nodiscard]] inline HeliotisRangeRejection::Reason anomalyReason(const ShapeOutcome outcome) noexcept
{
    switch (outcome) {
    case ShapeOutcome::Truncated: return HeliotisRangeRejection::Reason::TruncatedData;
    case ShapeOutcome::Excess: return HeliotisRangeRejection::Reason::InvalidLayout;
    case ShapeOutcome::UnusableDimensions: return HeliotisRangeRejection::Reason::InvalidDimensions;
    case ShapeOutcome::Exact: break;
    }
    return HeliotisRangeRejection::Reason::None;
}

/** Samples that fit whole into @p bytes, and the bytes left over. */
struct WholeSamples {
    std::uint64_t samples = 0;
    std::uint64_t remainderBytes = 0;
};

[[nodiscard]] inline WholeSamples wholeSamples(const std::uint64_t bytes, const std::size_t sampleSize) noexcept
{
    if (sampleSize == 0U) return {0U, bytes};
    return {bytes / sampleSize, bytes % sampleSize};
}

/** Copies delivered bytes into a raw-byte payload; the part then publishes as raw Mono8. */
[[nodiscard]] inline std::vector<std::uint8_t> rawBytes(const void* data, const std::uint64_t bytes)
{
    const auto* begin = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(begin, begin + static_cast<std::size_t>(bytes));
}

} // namespace heliotis::partfit
