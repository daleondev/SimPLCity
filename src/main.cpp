#include "hal/board/board.hpp"
#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
    using namespace std::chrono_literals;

    constexpr auto CYCLE_INTERVAL{ 500ms };
    constexpr auto FAILURE_BLINK_INTERVAL{ 100ms };

    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool user_button_press_pending{};

    auto debug(std::string_view message) -> void
    {
        static_cast<void>(std::fwrite(message.data(), sizeof(char), message.size(), stdout));
        static_cast<void>(std::putchar('\n'));
        static_cast<void>(std::fflush(stdout));
    }

    template<typename... Arguments>
        requires(sizeof...(Arguments) > 0U)
    auto debug(const char* pattern, Arguments... arguments) -> void
    {
        static_cast<void>(std::printf(pattern, arguments...));
        static_cast<void>(std::putchar('\n'));
        static_cast<void>(std::fflush(stdout));
    }

    auto report_user_button_press() -> void
    {
        static const auto yellow_led{ hal::board::createLed(hal::board::LedId::Yellow) };
        if (yellow_led == nullptr) {
            Error_Handler();
        }

        if (user_button_press_pending.exchange(false, std::memory_order_acq_rel)) {
            debug("[input] user button pressed");
            yellow_led->toggle();
        }
    }

    [[nodiscard]] auto run_filex_standard_library_sample() -> bool
    {
#if defined(HAL_PLATFORM_STM32)
        namespace fs = std::filesystem;
        constexpr std::string_view CONTENT{ "FileX through std::fstream\r\n" };
        const fs::path directory{ "/sample" };
        const fs::path file{ directory / "roundtrip.txt" };
        std::error_code error;
        static_cast<void>(fs::create_directory(directory, error));
        if (error) {
            return false;
        }
        std::string input(CONTENT.size(), '\0');
        {
            std::fstream stream{ file, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc };
            stream.write(CONTENT.data(), static_cast<std::streamsize>(CONTENT.size()));
            stream.flush();
            stream.seekg(0);
            stream.read(input.data(), static_cast<std::streamsize>(input.size()));
            if (!stream || stream.gcount() != static_cast<std::streamsize>(CONTENT.size())) {
                return false;
            }
        }
        const bool valid{ input == CONTENT && fs::is_regular_file(file, error) && !error &&
                          fs::file_size(file, error) == CONTENT.size() && !error };
        const bool file_removed{ fs::remove(file, error) };
        const bool directory_removed{ !error && fs::remove(directory, error) };
        return valid && file_removed && directory_removed && !error;
#else
        return true;
#endif
    }

    [[noreturn]] auto indicate_failure() -> void
    {
        const auto red_led{ hal::board::createLed(hal::board::LedId::Red) };
        if (red_led == nullptr) {
            Error_Handler();
        }
        debug("[ethercat] TEST FAILED - red LED indicates failure");
        while (true) {
            report_user_button_press();
            red_led->toggle();
            std::this_thread::sleep_for(FAILURE_BLINK_INTERVAL);
        }
    }
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    if (!run_filex_standard_library_sample()) {
        debug("[filex] std::fstream/std::filesystem sample failed");
        indicate_failure();
    }
    debug("[filex] std::fstream/std::filesystem sample passed");
    std::println("test");

    const auto user_button{ hal::board::createButton(hal::board::ButtonId::User) };
    if (user_button == nullptr) {
        debug("[input] user button creation failed");
        indicate_failure();
    }
    user_button->setStateChangedCallback([](hal::device::IButton::State state) noexcept {
        if (state == hal::device::IButton::State::Pressed) {
            user_button_press_pending.store(true, std::memory_order_release);
        }
    });

    const auto green_led{ hal::board::createLed(hal::board::LedId::Green) };
    if (green_led == nullptr) {
        debug("[output] green LED creation failed");
        indicate_failure();
    }

    while (true) {
        report_user_button_press();
        green_led->toggle();
        std::this_thread::sleep_for(CYCLE_INTERVAL);
    }
    indicate_failure();
}
