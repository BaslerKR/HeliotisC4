#include "Internal/HeliotisAcquisitionPolicy.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using heliotis::internal::TriggerSelectorState;

TriggerSelectorState triggerState(const char* mode, const char* source = nullptr)
{
    TriggerSelectorState state;
    state.modeReadable = mode != nullptr;
    state.mode = mode ? mode : "";
    state.sourceReadable = source != nullptr;
    state.source = source ? source : "";
    if (!state.modeReadable) state.error = "mode unavailable";
    else if (!state.sourceReadable) state.error = "source unavailable";
    return state;
}

bool require(const bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main()
{
    using heliotis::internal::FrameStartRoute;
    using heliotis::internal::RecordingStartRoute;
    using heliotis::internal::evaluateAcquisitionTriggerPlan;

    const auto canonical = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On", "Software"), triggerState("On", "Stage"));
    if (!require(canonical.valid && canonical.usesHostSoftwareTrigger()
            && !canonical.isFreeRun() && canonical.frameStartRoute == FrameStartRoute::Software
            && canonical.recordingStartRoute == RecordingStartRoute::Stage,
            "The canonical H8 Software/Stage route was not accepted.")) {
        return EXIT_FAILURE;
    }

    const auto freeRun = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Off"), triggerState("Off"));
    if (!require(freeRun.valid && freeRun.isFreeRun() && !freeRun.usesHostSoftwareTrigger(),
            "All-Off trigger routing must produce a true free-run plan.")) {
        return EXIT_FAILURE;
    }

    const auto stageGatedAutomaticFrame = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Off"), triggerState("On", "Stage"));
    if (!require(stageGatedAutomaticFrame.valid && !stageGatedAutomaticFrame.isFreeRun()
            && !stageGatedAutomaticFrame.warning.empty(),
            "FrameStart Off with RecordingStart Stage must be identified as stage-gated, not free-run.")) {
        return EXIT_FAILURE;
    }

    const auto externalFrame = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On", "DIO1"), triggerState("Off"));
    if (!require(externalFrame.valid && !externalFrame.isFreeRun()
            && !externalFrame.usesHostSoftwareTrigger()
            && externalFrame.frameStartRoute == FrameStartRoute::External,
            "A readable external FrameStart route must arm without exposing host software control.")) {
        return EXIT_FAILURE;
    }

    const auto externalRecording = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Off"), triggerState("On", "FI2"));
    if (!require(externalRecording.valid && !externalRecording.isFreeRun()
            && externalRecording.recordingStartRoute == RecordingStartRoute::External
            && !externalRecording.warning.empty(),
            "An external RecordingStart gate must remain valid but must not be labelled free-run.")) {
        return EXIT_FAILURE;
    }

    const auto deprecatedAcquisition = evaluateAcquisitionTriggerPlan(
        triggerState("On", "DIO1"), triggerState("Off"), triggerState("Off"));
    if (!require(!deprecatedAcquisition.valid
            && deprecatedAcquisition.error.find("AcquisitionStart=On") != std::string::npos,
            "Every enabled deprecated AcquisitionStart route must be rejected.")) {
        return EXIT_FAILURE;
    }

    const auto unreadableFrameSource = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On"), triggerState("Off"));
    if (!require(!unreadableFrameSource.valid
            && unreadableFrameSource.error.find("FrameStart is On") != std::string::npos,
            "An enabled FrameStart with an unreadable source must be rejected.")) {
        return EXIT_FAILURE;
    }

    const auto emptyFrameSource = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On", ""), triggerState("Off"));
    if (!require(!emptyFrameSource.valid
            && emptyFrameSource.error.find("FrameStart is On") != std::string::npos,
            "An enabled FrameStart with an empty source must be rejected.")) {
        return EXIT_FAILURE;
    }

    const auto softwareRecording = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On", "Software"), triggerState("On", "Software"));
    if (!require(!softwareRecording.valid
            && softwareRecording.error.find("RecordingStart=On/Software") != std::string::npos,
            "The unsupported second software-command route must be rejected.")) {
        return EXIT_FAILURE;
    }

    const auto unreadableRecordingSource = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Off"), triggerState("On"));
    if (!require(!unreadableRecordingSource.valid
            && unreadableRecordingSource.error.find("RecordingStart is On") != std::string::npos,
            "An enabled RecordingStart with an unreadable source must be rejected.")) {
        return EXIT_FAILURE;
    }

    const auto invalidMode = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Enabled", "Software"), triggerState("Off"));
    if (!require(!invalidMode.valid
            && invalidMode.error.find("unsupported TriggerMode") != std::string::npos,
            "An unknown TriggerMode value must fail closed.")) {
        return EXIT_FAILURE;
    }

    const auto deprecatedAuto = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("On", "Auto"), triggerState("On", "Auto"));
    if (!require(deprecatedAuto.valid && deprecatedAuto.isFreeRun()
            && deprecatedAuto.warning.find("deprecated") != std::string::npos,
            "Deprecated Auto sources must remain compatible while directing users to TriggerMode Off.")) {
        return EXIT_FAILURE;
    }

    const auto missingRecordingMode = evaluateAcquisitionTriggerPlan(
        triggerState("Off"), triggerState("Off"), triggerState(nullptr));
    if (!require(!missingRecordingMode.valid
            && missingRecordingMode.error.find("RecordingStart TriggerMode") != std::string::npos,
            "An uninspectable RecordingStart gate must fail closed.")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
