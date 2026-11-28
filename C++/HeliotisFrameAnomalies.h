#pragma once

/**
 * @file HeliotisFrameAnomalies.h
 * @brief Bounded, Qt-free evidence for frame data that was fitted instead of rejected.
 *
 * The producer never rejects device data for a validation reason. A part that is
 * short, longer than declared, shaped unexpectedly or in an unknown format is fitted
 * into the GraphicsEngine format and published; the deviation is counted here. Only
 * fixed numeric evidence is stored, so recording an anomaly never allocates and
 * never retains SDK strings or sample payload.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace heliotis {

/**
 * @brief Evidence for one deviation observed while assembling a requested range.
 *
 * The reason set mirrors `DeviceRangeRejection` from the host contract so the
 * runtime owner can publish the exact enum without this adapter including Qt.
 * The same record describes a rule that stopped a requested range (a real
 * failure: nothing to publish, a size limit or a storage refusal) and a deviation
 * that was fitted and published; `rangeRetained` and `imageRetained` tell which.
 */
struct HeliotisRangeRejection {
    enum class Reason : std::uint32_t {
        None,
        MissingRange,
        MissingData,
        InvalidDimensions,
        UnsupportedFormat,
        InvalidLayout,
        StrideBelowMinimum,
        TruncatedData,
        MissingCalibration,
        InvalidCalibration,
        UnsupportedGeometry,
        UnsupportedUnit,
        SizeLimitExceeded,
        StorageRefused
    };

    static constexpr auto noPart = (std::numeric_limits<std::uint32_t>::max)();

    Reason reason = Reason::None;
    std::uint32_t partCount = 0;
    std::uint32_t affectedPart = noPart;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameSequence = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    /** Destination bytes calculated by GraphicsRangeFrameWriter, even on refusal. */
    std::uint64_t requestedStorageBytes = 0;
    /** Resources assembled from the same input, independent of the affected range. */
    bool imageRetained = false;
    bool rangeRetained = false;
};

/**
 * @brief Fixed-capacity per-frame record of fitted deviations.
 *
 * Entries past `capacity` only increment `overflowByReason`, so a frame full of
 * anomalies still costs no allocation and loses no count. `affectedPart` is the
 * part ordinal in the SDK buffer for deviations seen while copying a buffer and
 * the ordinal in `Frame::parts` for deviations seen while assembling a frame.
 * For a part-count mismatch `readableBytes` is the authoritative part count and
 * `requiredBytes` the count the chunk metadata declared.
 */
struct HeliotisFrameAnomalies {
    static constexpr std::size_t capacity = 8;
    static constexpr std::size_t reasonCount =
        static_cast<std::size_t>(HeliotisRangeRejection::Reason::StorageRefused) + 1U;

    std::array<HeliotisRangeRejection, capacity> entries{};
    std::size_t count = 0;
    std::array<std::size_t, reasonCount> overflowByReason{};
    std::size_t overflow = 0;

    void add(const HeliotisRangeRejection& evidence) noexcept
    {
        const auto reason = static_cast<std::size_t>(evidence.reason);
        if (reason == 0U || reason >= reasonCount) return;
        if (count < entries.size()) {
            entries[count++] = evidence;
        } else {
            ++overflow;
            ++overflowByReason[reason];
        }
    }

    /** Builds and records one deviation. */
    void note(const HeliotisRangeRejection::Reason reason,
              const std::uint32_t affectedPart, const std::uint32_t partCount,
              const std::uint32_t width, const std::uint32_t height,
              const std::uint64_t readableBytes, const std::uint64_t requiredBytes) noexcept
    {
        HeliotisRangeRejection evidence;
        evidence.reason = reason;
        evidence.affectedPart = affectedPart;
        evidence.partCount = partCount;
        evidence.width = width;
        evidence.height = height;
        evidence.readableBytes = readableBytes;
        evidence.requiredBytes = requiredBytes;
        add(evidence);
    }

    /** Copies another record's entries and overflow counts into this one. */
    void merge(const HeliotisFrameAnomalies& other) noexcept
    {
        for (std::size_t index = 0; index < other.count; ++index) add(other.entries[index]);
        for (std::size_t reason = 0; reason < other.overflowByReason.size(); ++reason) {
            overflowByReason[reason] += other.overflowByReason[reason];
        }
        overflow += other.overflow;
    }

    /** Every recorded deviation, including those past the entry capacity. */
    [[nodiscard]] std::size_t total() const noexcept { return count + overflow; }
};

} // namespace heliotis
