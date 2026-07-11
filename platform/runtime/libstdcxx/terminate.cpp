#include "hal/hal.hpp"

#include <exception>

namespace
{
    [[noreturn]] auto runtime_terminate_handler() noexcept -> void
    {
        hal::panic("Unhandled C++ exception");
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
