#pragma once

#include <string>

namespace heliotis::internal {

/** Describes one selector-scoped TriggerMode/TriggerSource pair. */
struct TriggerSelectorState {
    bool modeReadable = false;
    bool sourceReadable = false;
    std::string mode;
    std::string source;
    std::string error;
};

/** Identifies how the device starts each frame. */
enum class FrameStartRoute {
    Automatic,
    Software,
    External
};

/** Identifies how the device starts sensor data recording. */
enum class RecordingStartRoute {
    Automatic,
    Stage,
    External
};

/**
 * Holds the validated H8 trigger route used by one acquisition arm.
 *
 * @note A valid free-run plan requires AcquisitionStart, FrameStart, and
 *       RecordingStart to have no remaining trigger gate.
 */
struct AcquisitionTriggerPlan {
    bool valid = false;
    FrameStartRoute frameStartRoute = FrameStartRoute::Automatic;
    RecordingStartRoute recordingStartRoute = RecordingStartRoute::Automatic;
    std::string frameStartSource;
    std::string recordingStartSource;
    std::string error;
    std::string warning;

    /** Returns whether the host must issue FrameStart TriggerSoftware commands. */
    [[nodiscard]] bool usesHostSoftwareTrigger() const noexcept;
    /** Returns whether both frame and recording gates are automatic. */
    [[nodiscard]] bool isFreeRun() const noexcept;
    /** Returns a stable diagnostic description of the selected route. */
    [[nodiscard]] std::string summary() const;
};

/**
 * Validates the three H8 acquisition trigger selectors and derives worker behavior.
 *
 * @param acquisitionStart AcquisitionStart selector state.
 * @param frameStart FrameStart selector state.
 * @param recordingStart RecordingStart selector state.
 * @return A valid plan or an actionable configuration error.
 * @note AcquisitionStart must be Off because C4Utility documents it as
 *       deprecated. RecordingStart software control is rejected because the
 *       host exposes one software-command path dedicated to FrameStart.
 */
[[nodiscard]] AcquisitionTriggerPlan evaluateAcquisitionTriggerPlan(
    const TriggerSelectorState& acquisitionStart,
    const TriggerSelectorState& frameStart,
    const TriggerSelectorState& recordingStart);

} // namespace heliotis::internal
