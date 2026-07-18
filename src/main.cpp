#include "hal/board/board.hpp"
#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include "pneumo/pneumo.hpp"
#include "runtime/thread.hpp"

#include <chrono>
#include <print>
#include <thread>
#include <utility>

namespace
{
    using namespace pnm::units::literals;

    constexpr auto CYCLE_INTERVAL{ 500ms };
    constexpr auto FAILURE_BLINK_INTERVAL{ 100ms };

    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool user_button_press_pending{};

    auto report_user_button_press() -> void
    {
        static const auto yellow_led{ hal::board::createLed(hal::board::LedId::Yellow) };
        if (!yellow_led) {
            throw(std::runtime_error("yellow LED creation failed"));
        }

        if (user_button_press_pending.exchange(false, std::memory_order_acq_rel)) {
            pnm::log::info("[input] user button pressed");
            yellow_led->toggle();
        }
    }
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    runtime::thread::publish_attributes({ .stack_size = 8192UZ });
    pnm::log::initialize();

    const auto user_button{ hal::board::createButton(hal::board::ButtonId::User) };
    if (!user_button) {
        throw(std::runtime_error("user button creation failed"));
    }
    user_button->setStateChangedCallback([](hal::device::IButton::State state) noexcept {
        if (state == hal::device::IButton::State::Pressed) {
            user_button_press_pending.store(true, std::memory_order_release);
        }
    });

    const auto green_led{ hal::board::createLed(hal::board::LedId::Green) };
    if (!green_led) {
        throw(std::runtime_error("green LED creation failed"));
    }

    while (true) {
        report_user_button_press();
        green_led->toggle();
        std::this_thread::sleep_for(CYCLE_INTERVAL);
        pnm::log::info("Hello World!");
    }

    std::unreachable();
}
