#include "HeliotisGraphicsFrameStream.h"
#include "HeliotisC4System.h"
#include "engine/GraphicsFrameContext.h"
#include <atomic>
#include <functional>

#include <iostream>
#include <stdexcept>

namespace {
/** Managed test backing that tracks live references and enforces exact sample limits. */
class TrackedManagedAllocation final : public GraphicsAllocation
{
public:
    [[nodiscard]] static GraphicsAllocation* create(
        const std::size_t capacityBytes,
        const std::uint64_t domainId,
        std::atomic<std::size_t>* const liveCount) noexcept
    {
        auto* rawBuffer = new (std::nothrow) std::byte[capacityBytes + 64U];
        if (!rawBuffer) return nullptr;
        const auto raw = reinterpret_cast<std::uintptr_t>(rawBuffer);
        auto* aligned = reinterpret_cast<std::byte*>((raw + 63U) & ~std::uintptr_t(63U));
        auto* alloc = new (std::nothrow) TrackedManagedAllocation(
            rawBuffer, aligned, capacityBytes, domainId, liveCount);
        if (!alloc) delete[] rawBuffer;
        return alloc;
    }

    ~TrackedManagedAllocation() override
    {
        if (_liveCount) _liveCount->fetch_sub(1U, std::memory_order_relaxed);
    }

    void destroy() noexcept override
    {
        delete this;
    }

private:
    TrackedManagedAllocation(
        std::byte* rawBuffer,
        std::byte* alignedBase,
        std::size_t capacityBytes,
        std::uint64_t domainId,
        std::atomic<std::size_t>* liveCount) noexcept
        : GraphicsAllocation(alignedBase, capacityBytes, GraphicsAllocationOrigin::Managed, 1U,
                             GraphicsAllocationState::Writable, domainId)
        , _buffer(rawBuffer)
        , _liveCount(liveCount)
    {
        if (_liveCount) _liveCount->fetch_add(1U, std::memory_order_relaxed);
    }

    std::unique_ptr<std::byte[]> _buffer;
    std::atomic<std::size_t>* _liveCount = nullptr;
};

class TrackedFrameAllocator final : public GraphicsFrameAllocator
{
public:
    TrackedFrameAllocator(
        GraphicsAllocationRef backing,
        std::size_t sampleLimit,
        std::uint64_t domainId,
        std::atomic<std::size_t>* reserveCalls,
        std::atomic<std::size_t>* usedBytes, std::function<void()>* onAllocate)
        : _backing(std::move(backing))
        , _sampleLimit(sampleLimit)
        , _domainId(domainId)
        , _reserveCalls(reserveCalls)
        , _usedBytes(usedBytes)
        , _onAllocate(onAllocate)
    {
    }

    GraphicsResult<GraphicsAllocationSlice> allocate(
        const std::size_t bytes, const std::size_t alignment) noexcept override
    {
        if (_reserveCalls) _reserveCalls->fetch_add(1U, std::memory_order_relaxed);
        if (_onAllocate && *_onAllocate) (*_onAllocate)();
        if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
            return GraphicsFrameError::InvalidInput;
        }
        const auto baseAddr = reinterpret_cast<std::uintptr_t>(_backing->base());
        const auto curAddr = baseAddr + static_cast<std::uintptr_t>(_cursor);
        const std::size_t alignMask = alignment - 1U;
        const std::size_t misalignment = static_cast<std::size_t>(curAddr & alignMask);
        const std::size_t pad = (misalignment == 0U) ? 0U : (alignment - misalignment);
        if (pad > _sampleLimit || _cursor + pad > _sampleLimit
            || bytes > _sampleLimit - (_cursor + pad)) {
            return GraphicsFrameError::Capacity;
        }
        const std::size_t offset = _cursor + pad;
        _cursor = offset + bytes;
        if (_usedBytes) _usedBytes->store(_cursor, std::memory_order_relaxed);
        return GraphicsAllocationSlice{_backing, offset, bytes};
    }

    bool isManaged() const noexcept override { return true; }
    std::uint64_t domainId() const noexcept override { return _domainId; }

private:
    GraphicsAllocationRef _backing;
    std::size_t _sampleLimit = 0U;
    std::uint64_t _domainId = 0U;
    std::size_t _cursor = 0U;
    std::atomic<std::size_t>* _reserveCalls = nullptr;
    std::atomic<std::size_t>* _usedBytes = nullptr;
    std::function<void()>* _onAllocate = nullptr;
};

/** Test context source mirroring WorkerFrameContextSource + FrameScopeAllocator invariants. */
class TrackedContextSource final : public GraphicsFrameContextSource
{
public:
    static constexpr std::uint64_t kDomainId = 7001U;

    std::atomic<std::size_t> calls{0};
    std::atomic<std::size_t> reserveCalls{0};
    std::atomic<std::size_t> requestedBytes{0};
    std::atomic<std::size_t> usedBytes{0};
    std::atomic<std::size_t> liveAllocations{0};
    GraphicsFrameError nextError = GraphicsFrameError::None;
    bool refuse = false;
    std::size_t singleCalls = 0;
    bool contains(const void* ptr) const {
        const auto p = reinterpret_cast<std::uintptr_t>(ptr);
        const auto base = reinterpret_cast<std::uintptr_t>(lastBackingBase);
        return base && p >= base && p - base < requestedBytes.load();
    }
    const std::uint8_t* imageAt(std::size_t) const { return reinterpret_cast<const std::uint8_t*>(lastBackingBase); }
    std::size_t budgetDelta = 0; // Subtracted from allocated limit to test reserve overflow.
    const std::byte* lastBackingBase = nullptr;
    std::function<void()> onBeginFrame;
    std::function<void()> onAllocate;

    GraphicsResult<GraphicsFrameContext> beginFrame(const std::size_t sampleBytes) noexcept override
    {
        calls.fetch_add(1U, std::memory_order_relaxed);
        requestedBytes.store(sampleBytes, std::memory_order_relaxed);
        usedBytes.store(0U, std::memory_order_relaxed);
        if (onBeginFrame) onBeginFrame();
        if (refuse) return GraphicsFrameError::ClassRefused;
        if (nextError != GraphicsFrameError::None) {
            return nextError;
        }
        if (sampleBytes == 0U) {
            return GraphicsFrameError::InvalidInput;
        }
        const std::size_t limit = sampleBytes > budgetDelta ? sampleBytes - budgetDelta : 0U;
        auto* rawAlloc = TrackedManagedAllocation::create(limit, kDomainId, &liveAllocations);
        if (!rawAlloc) return GraphicsFrameError::Allocation;
        auto backing = GraphicsAllocationRef::adoptNew(rawAlloc);
        lastBackingBase = backing->base();
        auto allocator = std::make_shared<TrackedFrameAllocator>(
            std::move(backing), limit, kDomainId, &reserveCalls, &usedBytes, &onAllocate);
        return GraphicsFrameContext(std::move(allocator));
    }
};

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
bool HeliotisC4Device::isAcquiring() const { return false; }
void HeliotisC4Device::requestStopAcquisition() noexcept {}
void HeliotisC4Device::stopAcquisition() {}
}

int main()
{
    heliotis::HeliotisC4Device device(nullptr);
    int receipts = 0;
    int deliveries = 0;
    int storageFailures = 0;
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
        auto source = std::make_shared<TrackedContextSource>();
        stream.setStorageFailureHandler([&]() { ++storageFailures; });
        stream.setFrameContextSource(source);
        if (!stream.start(heliotis::HeliotisC4Device::AcquisitionMode::Continuous)) return 1;

        heliotis::Frame frame;
        frame.parts = {{heliotis::FramePartKind::Range, "Range", "Mono16", 1, 1, 16,
                        std::vector<std::uint16_t>{42U}}};
        deviceCallback(heliotis::Frame(frame));
        if (receipts != 1 || deliveries != 1 || !receiptPrecedesDelivery) return 2;
        if (stream.runtimeDiagnostics().delivered != 1 || stream.runtimeDiagnostics().failed != 0) return 7;

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
        if (stream.runtimeDiagnostics().delivered != 2 || stream.runtimeDiagnostics().failed != 2) return 8;
        throwOnDelivery = false;
        auto oldCallback = deviceCallback;
        source->nextError = GraphicsFrameError::NoReadyCredit;
        const auto reservedBefore = source->reserveCalls.load();
        deviceCallback(heliotis::Frame(frame));
        if (stream.droppedFrames() != 1 || source->reserveCalls != reservedBefore || deliveries != 3) return 11;
        source->nextError = GraphicsFrameError::Disconnected;
        deviceCallback(heliotis::Frame(frame));
        if (!stream.canStart() || stream.droppedFrames() != 1 || deliveries != 3) return 12;
        // A refused class or capacity drops and counts only that frame; the run is not refused
        // and the message is surfaced once per run.
        const auto failedBeforeRefusal = stream.runtimeDiagnostics().failed;
        source->nextError = GraphicsFrameError::ClassRefused;
        deviceCallback(heliotis::Frame(frame));
        source->nextError = GraphicsFrameError::Capacity;
        deviceCallback(heliotis::Frame(frame));
        if (!stream.canStart() || deliveries != 3) return 13;
        if (stream.runtimeDiagnostics().failed != failedBeforeRefusal + 2) return 17;
        if (storageFailures != 1) return 18;
        source->nextError = GraphicsFrameError::None;
        deviceCallback(heliotis::Frame(frame));
        if (deliveries != 4 || stream.runtimeDiagnostics().delivered != 3) return 19;
        auto nextSource = std::make_shared<TrackedContextSource>();
        stream.setFrameContextSource(nextSource);
        if (!stream.start(heliotis::AcquisitionMode::Continuous)) return 14;
        oldCallback(heliotis::Frame(frame));
        if (deliveries != 4) return 15;
        nextSource->onAllocate = [&]() { stream.requestStop(); };
        deviceCallback(heliotis::Frame(frame));
        if (deliveries != 4 || nextSource->liveAllocations != 0 || nextSource->reserveCalls == 0) return 16;
        nextSource->onAllocate = {};
        // Waiting without a frame leaves diagnostics unchanged; starting a new run resets them.
        if (!stream.start(heliotis::AcquisitionMode::Continuous)) return 9;
        if (stream.runtimeDiagnostics().delivered != 0 || stream.runtimeDiagnostics().failed != 0) return 10;
    }
    // A callback retained by a dispatcher must be harmless after stream destruction.
    deviceCallback(heliotis::Frame{});
    if (deliveries != 4) return 6;
    std::cout << "Frame receipt, conversion failure, callback exceptions, and shutdown passed.\n";
}
