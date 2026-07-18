#include "hal/hal.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>

namespace
{
    template<std::size_t Size>
    auto append(std::array<char, Size>& buffer, std::size_t& length, const char* text) noexcept -> void
    {
        if (!text || length >= Size) {
            return;
        }

        auto available{ Size - length - 1U };
        auto str{ std::ranges::subrange(text, std::unreachable_sentinel) |
                  std::views::take_while([](char c) noexcept { return c != '\0'; }) |
                  std::views::take(available) };
        auto result{ std::ranges::copy(str, buffer.begin() + length) };

        length = static_cast<std::size_t>(std::distance(buffer.begin(), result.out));
        buffer[length] = '\0';
    }

    [[noreturn]] auto runtime_terminate_handler() noexcept -> void
    {
        std::array<char, 256UZ> message{};
        auto length{ 0UZ };

        auto exception{ std::current_exception() };
        if (exception) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& e) {
                append(message, length, "Unhandled exception: ");
                append(message, length, e.what());
            } catch (...) {
                append(message, length, "Unhandled non-std exception");
            }
        }
        else {
            append(message, length, "Runtime terminated without exception");
        }

        hal::panic(message.data());
    }
}

extern "C" auto runtime_install_terminate_handler() noexcept -> void
{
    std::set_terminate(runtime_terminate_handler);
}

namespace
{
    using PreinitFunction = void (*)() noexcept;

    // The ELF preinit array runs before ordinary C++ global constructors.
    // Both the Linux loader and STM32's __libc_init_array process this section.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    [[gnu::used, gnu::section(".preinit_array")]] PreinitFunction install_terminate_handler{
        runtime_install_terminate_handler
    };
}
