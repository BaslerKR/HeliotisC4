#pragma once

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"

#include <optional>

namespace heliotis {

class HeliotisC4GraphicsFrameAdapter final
    : public GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>
{
public:
    HeliotisC4GraphicsFrameAdapter() = default;
    ~HeliotisC4GraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>::convertFrame;

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request) const;
};

} // namespace heliotis
