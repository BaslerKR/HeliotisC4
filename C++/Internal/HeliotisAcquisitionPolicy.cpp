#include "Internal/HeliotisAcquisitionPolicy.h"

#include <sstream>

namespace heliotis::internal {
namespace {

/** Appends one warning without losing an earlier compatibility warning. */
void appendWarning(std::string* destination, const std::string& warning)
{
    if (!destination || warning.empty()) return;
    if (!destination->empty()) *destination += " ";
    *destination += warning;
}

/** Returns a diagnostic label for a frame-start route. */
std::string frameStartRouteName(const AcquisitionTriggerPlan& plan)
{
    switch (plan.frameStartRoute) {
    case FrameStartRoute::Automatic:
        return "automatic";
    case FrameStartRoute::Software:
        return "software(" + plan.frameStartSource + ")";
    case FrameStartRoute::External:
        return "external(" + plan.frameStartSource + ")";
    }
    return "unknown";
}

/** Returns a diagnostic label for a recording-start route. */
std::string recordingStartRouteName(const AcquisitionTriggerPlan& plan)
{
    switch (plan.recordingStartRoute) {
    case RecordingStartRoute::Automatic:
        return "automatic";
    case RecordingStartRoute::Stage:
        return "stage(" + plan.recordingStartSource + ")";
    case RecordingStartRoute::External:
        return "external(" + plan.recordingStartSource + ")";
    }
    return "unknown";
}

/** Validates that a selector exposes an On/Off TriggerMode. */
bool validateMode(
    const TriggerSelectorState& state,
    const char* selector,
    AcquisitionTriggerPlan* plan)
{
    if (!plan) return false;
    if (!state.modeReadable) {
        plan->error = "Could not inspect " + std::string(selector)
            + " TriggerMode: " + (state.error.empty() ? std::string("unavailable") : state.error) + ".";
        return false;
    }
    if (state.mode != "On" && state.mode != "Off") {
        plan->error = std::string(selector) + " has unsupported TriggerMode=" + state.mode
            + "; expected On or Off.";
        return false;
    }
    return true;
}

} // namespace

bool AcquisitionTriggerPlan::usesHostSoftwareTrigger() const noexcept
{
    return valid && frameStartRoute == FrameStartRoute::Software;
}

bool AcquisitionTriggerPlan::isFreeRun() const noexcept
{
    return valid && !acquisitionStartEnabled
        && frameStartRoute == FrameStartRoute::Automatic
        && recordingStartRoute == RecordingStartRoute::Automatic;
}

std::string AcquisitionTriggerPlan::summary() const
{
    if (!valid) return "invalid=" + error;
    std::ostringstream stream;
    stream << "AcquisitionStart=" << (acquisitionStartEnabled ? "on" : "off")
           << ", FrameStart=" << frameStartRouteName(*this)
           << ", RecordingStart=" << recordingStartRouteName(*this)
           << ", hostSoftwareTrigger=" << (usesHostSoftwareTrigger() ? "true" : "false")
           << ", freeRun=" << (isFreeRun() ? "true" : "false");
    if (!warning.empty()) stream << ", warning=" << warning;
    return stream.str();
}

AcquisitionTriggerPlan evaluateAcquisitionTriggerPlan(
    const TriggerSelectorState& acquisitionStart,
    const TriggerSelectorState& frameStart,
    const TriggerSelectorState& recordingStart)
{
    AcquisitionTriggerPlan plan;
    if (!validateMode(acquisitionStart, "AcquisitionStart", &plan)
        || !validateMode(frameStart, "FrameStart", &plan)
        || !validateMode(recordingStart, "RecordingStart", &plan)) {
        return plan;
    }

    plan.acquisitionStartEnabled = acquisitionStart.mode == "On";

    if (frameStart.mode == "Off") {
        plan.frameStartRoute = FrameStartRoute::Automatic;
    } else {
        if (!frameStart.sourceReadable || frameStart.source.empty()) {
            plan.error = "FrameStart is On but its TriggerSource cannot be read. Set FrameStart=Off for "
                "automatic frames or select a readable source before arming.";
            return plan;
        }
        plan.frameStartSource = frameStart.source;
        if (frameStart.source == "Software") {
            plan.frameStartRoute = FrameStartRoute::Software;
        } else if (frameStart.source == "Auto") {
            plan.frameStartRoute = FrameStartRoute::Automatic;
            appendWarning(&plan.warning,
                "FrameStart TriggerSource=Auto is deprecated; use TriggerMode=Off for automatic frames.");
        } else {
            plan.frameStartRoute = FrameStartRoute::External;
        }
    }

    if (recordingStart.mode == "Off") {
        plan.recordingStartRoute = RecordingStartRoute::Automatic;
    } else {
        if (!recordingStart.sourceReadable || recordingStart.source.empty()) {
            plan.error = "RecordingStart is On but its TriggerSource cannot be read. Set RecordingStart=Off "
                "for automatic recording or select a readable source before arming.";
            return plan;
        }
        plan.recordingStartSource = recordingStart.source;
        if (recordingStart.source == "Software") {
            plan.error = "RecordingStart=On/Software is unsupported by the host's FrameStart-only software "
                "command path. Set RecordingStart=Off for automatic recording or use Stage/external control.";
            return plan;
        }
        if (recordingStart.source == "Auto") {
            plan.recordingStartRoute = RecordingStartRoute::Automatic;
            appendWarning(&plan.warning,
                "RecordingStart TriggerSource=Auto is deprecated; use TriggerMode=Off for automatic recording.");
        } else if (recordingStart.source == "Stage") {
            plan.recordingStartRoute = RecordingStartRoute::Stage;
        } else {
            plan.recordingStartRoute = RecordingStartRoute::External;
        }
    }

    plan.valid = true;
    if (plan.frameStartRoute == FrameStartRoute::Automatic
        && plan.recordingStartRoute != RecordingStartRoute::Automatic) {
        appendWarning(&plan.warning,
            "FrameStart is automatic while RecordingStart remains stage/external-gated; this is not free-run, "
            "and repeated frames depend on that recording source.");
    }
    return plan;
}

} // namespace heliotis::internal
