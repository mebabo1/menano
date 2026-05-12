#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>

namespace Mini {

void setSemaphoreCapabilities(bool fd, bool win32);

class Semaphore {
public:
    Semaphore() noexcept = default;

    // internal-only constructor
    Semaphore(VkDevice device);

    // KEEP for ABI compatibility (but internally ignored)
    Semaphore(VkDevice device, int* fd);

    [[nodiscard]] VkSemaphore handle() const { return *semaphore; }

    Semaphore(const Semaphore&) noexcept = default;
    Semaphore& operator=(const Semaphore&) noexcept = default;
    Semaphore(Semaphore&&) noexcept = default;
    Semaphore& operator=(Semaphore&&) noexcept = default;
    ~Semaphore() = default;

private:
    std::shared_ptr<VkSemaphore> semaphore;
};

}
