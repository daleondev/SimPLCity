#include "hal/hal.hpp"

#include <cstdlib>
#include <string_view>

extern "C" [[noreturn]] void hal_panic_handler(const HalPanicInfo* info) noexcept
{
    const bool valid{ info != nullptr && info->message != nullptr && info->file != nullptr &&
                      info->function != nullptr && info->line != 0U &&
                      std::string_view{ info->message } == "panic override probe" };
    std::_Exit(valid ? 42 : 43);
}

int main()
{
    hal::panic("panic override probe");
}
