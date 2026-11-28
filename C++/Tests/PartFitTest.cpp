#include "Internal/HeliotisPartFit.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void require(const bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

} // namespace

int main()
{
    using heliotis::HeliotisFrameAnomalies;
    using heliotis::HeliotisRangeRejection;
    using Reason = HeliotisRangeRejection::Reason;
    using Outcome = heliotis::partfit::ShapeOutcome;

    // A delivered payload that matches the declared shape is left alone.
    const auto exact = heliotis::partfit::fitShape(true, 4U, 12U, 12U);
    require(exact.outcome == Outcome::Exact && exact.width == 4U && exact.height == 3U,
            "An exact payload must keep the declared shape.");
    require(heliotis::partfit::anomalyReason(exact.outcome) == Reason::None,
            "An exact payload is not an anomaly.");

    // A short payload keeps the declared shape; the consumer zero-fills the tail.
    const auto shortShape = heliotis::partfit::fitShape(true, 4U, 12U, 5U);
    require(shortShape.outcome == Outcome::Truncated && shortShape.width == 4U && shortShape.height == 3U,
            "A short payload must keep the declared shape instead of becoming a preview.");
    require(heliotis::partfit::anomalyReason(shortShape.outcome) == Reason::TruncatedData,
            "A short payload is counted as TruncatedData.");

    // Extra samples keep the declared shape too; the consumer ignores the excess.
    const auto excess = heliotis::partfit::fitShape(true, 4U, 12U, 20U);
    require(excess.outcome == Outcome::Excess && excess.width == 4U && excess.height == 3U,
            "A longer payload must keep the declared shape.");
    require(heliotis::partfit::anomalyReason(excess.outcome) == Reason::InvalidLayout,
            "A longer payload is counted as InvalidLayout.");

    // Without a usable declaration the delivered samples form one row.
    const auto undeclared = heliotis::partfit::fitShape(false, 0U, 0U, 7U);
    require(undeclared.outcome == Outcome::UnusableDimensions && undeclared.width == 7U && undeclared.height == 1U,
            "Undeclared dimensions must become a one-row preview of the delivered samples.");
    require(heliotis::partfit::anomalyReason(undeclared.outcome) == Reason::InvalidDimensions,
            "Undeclared dimensions are counted as InvalidDimensions.");
    const auto inconsistent = heliotis::partfit::fitShape(true, 5U, 12U, 12U);
    require(inconsistent.outcome == Outcome::UnusableDimensions && inconsistent.width == 12U,
            "A declaration whose product does not divide by its width is not a usable shape.");
    require(heliotis::partfit::fitShape(false, 0U, 0U, 0U).width == 1U,
            "An empty delivery still yields a representable shape.");

    // Bytes that are not whole samples are kept verbatim as raw bytes.
    const auto whole = heliotis::partfit::wholeSamples(9U, sizeof(std::uint16_t));
    require(whole.samples == 4U && whole.remainderBytes == 1U, "Nine bytes hold four whole 16-bit samples.");
    const auto wholeDoubles = heliotis::partfit::wholeSamples(16U, sizeof(double));
    require(wholeDoubles.samples == 2U && wholeDoubles.remainderBytes == 0U, "Sixteen bytes hold two doubles.");
    const std::array<std::uint8_t, 3> source{1U, 2U, 3U};
    const auto bytes = heliotis::partfit::rawBytes(source.data(), source.size());
    require(bytes.size() == 3U && bytes[2] == 3U, "Raw bytes must be copied verbatim.");

    // The per-frame record has a fixed capacity and keeps counting past it.
    HeliotisFrameAnomalies record;
    for (std::uint32_t part = 0; part < 11U; ++part) {
        record.note(part % 2U == 0U ? Reason::TruncatedData : Reason::MissingData, part, 11U, 2U, 2U, 6U, 8U);
    }
    require(record.count == HeliotisFrameAnomalies::capacity && record.overflow == 3U && record.total() == 11U,
            "The record must hold eight entries and count the overflow.");
    require(record.overflowByReason[static_cast<std::size_t>(Reason::MissingData)] == 1U
                && record.overflowByReason[static_cast<std::size_t>(Reason::TruncatedData)] == 2U,
            "The overflow must be counted per reason.");
    require(record.entries[0].reason == Reason::TruncatedData && record.entries[1].affectedPart == 1U
                && record.entries[1].readableBytes == 6U && record.entries[1].requiredBytes == 8U,
            "Entries must keep their numeric evidence in order.");
    record.add(HeliotisRangeRejection{});
    require(record.total() == 11U, "An entry without a reason is not an anomaly.");

    HeliotisFrameAnomalies other;
    other.note(Reason::InvalidLayout, HeliotisRangeRejection::noPart, 3U, 0U, 0U, 3U, 4U);
    other.overflow = 2U;
    other.overflowByReason[static_cast<std::size_t>(Reason::UnsupportedFormat)] = 2U;
    HeliotisFrameAnomalies merged;
    merged.merge(other);
    require(merged.count == 1U && merged.overflow == 2U
                && merged.overflowByReason[static_cast<std::size_t>(Reason::UnsupportedFormat)] == 2U,
            "Merging must carry entries and overflow counts.");

    // A frame keeps shape-deviating parts deliverable; only completeness is reported by isValid().
    heliotis::Frame frame;
    frame.parts.push_back({heliotis::FramePartKind::Range, "Range", "Mono16", 2, 2, 16,
                           std::vector<std::uint16_t>{1U, 2U, 3U}});
    require(!frame.isValid() && frame.parts.front().sampleBytes() == 6U && frame.parts.front().sampleCount() == 3U,
            "A short part reports the bytes it really carries.");
    frame.parts.front().samples = std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U};
    require(frame.parts.front().sampleBytes() == 5U && frame.parts.front().sampleCount() == 5U,
            "A raw-byte part reports its byte count.");
    frame.parts.front().samples = std::vector<double>{1.0, 2.0};
    require(frame.parts.front().sampleBytes() == 16U, "A floating part reports eight bytes per sample.");

    return failures == 0 ? 0 : 1;
}
