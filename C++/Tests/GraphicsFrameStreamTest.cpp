#include "HeliotisC4GraphicsFrameAdapter.h"

#include <iostream>
#include <stdexcept>

namespace {
heliotis::HeliotisC4Device::FrameCallback deviceCallback;
}

// Replace only the device transport; exercise the real adapter stream without hardware.
namespace heliotis {
HeliotisC4Device::HeliotisC4Device(HeliotisC4System* system) : _system(system) {}
HeliotisC4Device::~HeliotisC4Device() = default;
bool HeliotisC4Device::startAcquisition(AcquisitionMode, FrameCallback callback, std::string*)
{
    deviceCallback = std::move(callback);
    return true;
}
void HeliotisC4Device::requestStopAcquisition() noexcept {}
void HeliotisC4Device::stopAcquisition() {}
}

int main()
{
    heliotis::HeliotisC4Device device(nullptr);
    int receipts = 0;
    int deliveries = 0;
    bool throwOnReceipt = false;
    bool throwOnDelivery = false;
    bool receiptPrecedesDelivery = true;
    {
        heliotis::HeliotisGraphicsFrameStream stream(&device,
            [&](GraphicsFrame&& frame, unsigned int sourceIndex) {
                receiptPrecedesDelivery = receiptPrecedesDelivery
                    && receipts > deliveries && frame.hasRangeFrame() && sourceIndex == 0U;
                ++deliveries;
                if (throwOnDelivery) throw std::runtime_error("consumer failure");
            },
            [&]() {
                ++receipts;
                if (throwOnReceipt) throw std::runtime_error("receipt failure");
            });
        if (!stream.start(heliotis::HeliotisC4Device::AcquisitionMode::Continuous)) return 1;

        heliotis::Frame frame;
        frame.parts = {{heliotis::FramePartKind::Range, "Range", "Mono16", 1, 1, 16,
                        std::vector<std::uint16_t>{42U}}};
        deviceCallback(heliotis::Frame(frame));
        if (receipts != 1 || deliveries != 1 || !receiptPrecedesDelivery) return 2;

        // An unconvertible buffer still acknowledges receipt exactly once.
        deviceCallback(heliotis::Frame{});
        if (receipts != 2 || deliveries != 1) return 3;

        throwOnReceipt = true;
        deviceCallback(heliotis::Frame(frame));
        if (receipts != 3 || deliveries != 2) return 4;
        throwOnReceipt = false;
        throwOnDelivery = true;
        deviceCallback(heliotis::Frame(frame));
        if (receipts != 4 || deliveries != 3) return 5;
    }
    // A callback retained by a dispatcher must be harmless after stream destruction.
    deviceCallback(heliotis::Frame{});
    if (receipts != 4 || deliveries != 3) return 6;
    std::cout << "Frame receipt, conversion failure, callback exceptions, and shutdown passed.\n";
}
