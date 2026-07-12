#include "hal/board/board.hpp"
#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include "pneumo/meta.hpp"

#include <chrono>
#include <print>
#include <thread>
#include <utility>

namespace
{
    using namespace std::chrono_literals;

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
            std::println("[input] user button pressed");
            yellow_led->toggle();
        }
    }
}

enum class TestEnum
{
    Hello,
    World
};

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
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
        std::println("{} {}",
                     pnm::meta::enumeration::enumerator_name<TestEnum::Hello>().data(),
                     pnm::meta::enumeration::enumerator_name<TestEnum::World>().data());
    }

    std::unreachable();
}
