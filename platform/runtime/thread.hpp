#pragma once

#include <cstddef>
#include <cstdint>

namespace runtime::thread
{
    struct Attributes
    {
        // ThreadX priorities are ordered from 0 (highest) to
        // TX_MAX_PRIORITIES - 1 (lowest). -1 selects the configured default.
        std::int32_t priority{ -1 };

        // Stack size in bytes. Zero selects the configured default.
        std::size_t stack_size{};
    };

    // Applies attributes to the next std::thread or std::jthread created by
    // the calling thread. Publishing again before creation replaces them.
    void publish_attributes(const Attributes& attributes) noexcept;
}
