#pragma once

#include "HeliotisC4.h"
#include "engine/Scene3DAdapter.h"

#include <optional>

namespace heliotis {

class HeliotisC4GraphicsSceneAdapter final
    : public Scene3DAdapter<HeliotisC4GraphicsSceneAdapter, Frame>
{
public:
    HeliotisC4GraphicsSceneAdapter() = default;
    ~HeliotisC4GraphicsSceneAdapter() = default;

    using Scene3DAdapter<HeliotisC4GraphicsSceneAdapter, Frame>::convert;

private:
    friend class Scene3DAdapter<HeliotisC4GraphicsSceneAdapter, Frame>;

    [[nodiscard]] std::optional<GraphicsScene3D> convertScene3D(
        const Frame& frame,
        const GraphicsScene3DRequest& request) const;
};

} // namespace heliotis
