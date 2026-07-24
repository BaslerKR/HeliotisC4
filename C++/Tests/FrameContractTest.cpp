#include "HeliotisC4.h"

#include <cstdint>
#include <iostream>

int main()
{
    heliotis::Frame frame;
    frame.sequence = 42;
    frame.frameId = "h8-test";
    frame.timestampNs = 42000000U;
    frame.parts.push_back({
        heliotis::FramePartKind::Range,
        "Range",
        "Coord3D_ABC32f",
        2,
        2,
        64,
        std::vector<double>{1.0, 2.0, 3.0, 4.0}
    });
    frame.parts.front().sampleScale = 0.5;

    if (!frame.isValid() || !frame.timestampNs.has_value() || frame.parts.front().sampleScale != 0.5)
    {
        std::cerr << "A complete Heliotis frame must be valid.\n";
        return 1;
    }

    frame.parts.front().samples = std::vector<std::uint16_t>{1U, 2U, 3U};
    if (frame.isValid())
    {
        std::cerr << "A frame with a mismatched part payload must be invalid.\n";
        return 1;
    }

    return 0;
}
