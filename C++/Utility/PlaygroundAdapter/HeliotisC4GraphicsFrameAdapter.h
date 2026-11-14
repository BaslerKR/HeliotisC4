#pragma once

#include "HeliotisC4.h"
#include "engine/GraphicsFrameAdapter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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

    /**
     * Storage for one image: returns at least `bytes` writable bytes, or empty to drop the image.
     * Must not wait; called on acquisition threads.
     */
    using FrameStorage = std::function<std::shared_ptr<std::uint8_t>(std::size_t bytes)>;
    /** Sets where intensity images are written; empty restores storage of the adapter's own. */
    void setFrameStorage(FrameStorage storage);

private:
    friend class GraphicsFrameAdapter<HeliotisC4GraphicsFrameAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Frame& frame,
        const GraphicsFrameRequest& request) const;

    mutable std::mutex _storageMutex;
    FrameStorage _storage;
};

} // namespace heliotis
