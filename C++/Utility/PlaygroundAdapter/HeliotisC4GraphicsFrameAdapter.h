#pragma once

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace heliotis {

class HeliotisC4GraphicsFrameAdapter final
    : public GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>
{
public:
    HeliotisC4GraphicsFrameAdapter() = default;
    ~HeliotisC4GraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>::convertFrame;

    /** Sets where intensity images are written; empty restores storage of the adapter's own. */
    void setFrameStorage(GraphicsImageStorage storage);

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request) const;

    mutable std::mutex _storageMutex;
    GraphicsImageStorage _storage;
};

} // namespace heliotis
