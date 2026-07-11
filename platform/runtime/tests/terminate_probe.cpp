#include "hal/hal.hpp"

#include <stdexcept>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{ argv[1] } == "error_handler") {
        Error_Handler();
    }

    if (argc == 2 && std::string_view{ argv[1] } == "thread") {
        std::thread throwing_thread{ [] { throw std::runtime_error{ "uncaught thread exception" }; } };
        throwing_thread.join();
        return 1;
    }

    throw std::runtime_error{ "uncaught application exception" };
}
