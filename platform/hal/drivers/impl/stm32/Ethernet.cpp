#include "Ethernet.hpp"

#include "ethernet_dma.h"
#include "hal/drivers/common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>

namespace
{
    constexpr std::size_t RX_SLOT_COUNT{ ETH_RX_DESC_CNT * 2U };
    constexpr std::uint32_t PHY_BASIC_STATUS_REGISTER{ 1U };
    constexpr std::uint32_t PHY_LINK_STATUS{ 1U << 2U };
    constexpr std::uint32_t PHY_SPECIAL_CONTROL_STATUS_REGISTER{ 31U };
    constexpr std::uint32_t PHY_SPEED_DUPLEX_MASK{ 0x001CU };
    constexpr std::uint32_t PHY_10_HALF_DUPLEX{ 0x0004U };
    constexpr std::uint32_t PHY_100_HALF_DUPLEX{ 0x0008U };
    constexpr std::uint32_t PHY_10_FULL_DUPLEX{ 0x0014U };
    constexpr std::uint32_t PHY_100_FULL_DUPLEX{ 0x0018U };

    struct RxSlot
    {
        ETH_BufferTypeDef link{};
        alignas(ETH_DMA_BUFFER_ALIGNMENT)
          std::array<std::byte, ETH_DMA_FRAME_BUFFER_SIZE> data{};
        bool allocated{};
    };

    std::array<RxSlot, RX_SLOT_COUNT> rx_slots ETH_DMA_BUFFER_ATTRIBUTE;
    std::array<std::byte, ETH_DMA_FRAME_BUFFER_SIZE> tx_frame ETH_DMA_BUFFER_ATTRIBUTE;

    CHAR state_mutex_name[] = "Ethernet state";
    CHAR receive_mutex_name[] = "Ethernet receive";
    CHAR transmit_mutex_name[] = "Ethernet transmit";

    class ThreadXLock
    {
      public:
        explicit ThreadXLock(TX_MUTEX& mutex)
          : m_mutex{ mutex }
        {
            if (tx_mutex_get(&m_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ~ThreadXLock()
        {
            if (tx_mutex_put(&m_mutex) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ThreadXLock(const ThreadXLock&) = delete;
        ThreadXLock& operator=(const ThreadXLock&) = delete;
        ThreadXLock(ThreadXLock&&) = delete;
        ThreadXLock& operator=(ThreadXLock&&) = delete;

      private:
        TX_MUTEX& m_mutex;
    };

    template<typename T = void>
    [[nodiscard]] auto error_result(std::errc error) noexcept -> util::Result<T>
    {
        return std::unexpected(std::make_error_code(error));
    }

    [[nodiscard]] auto timeout_milliseconds(std::chrono::milliseconds timeout) noexcept -> std::uint32_t
    {
        if (timeout.count() <= 0) {
            return 0U;
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(timeout.count()),
                                                                  std::numeric_limits<std::uint32_t>::max()));
    }

    [[nodiscard]] auto find_slot(const std::uint8_t* buffer) noexcept -> RxSlot*
    {
        const auto slot{ std::ranges::find_if(rx_slots, [&](const RxSlot& candidate) {
            return candidate.data.data() == reinterpret_cast<const std::byte*>(buffer);
        }) };
        return slot == rx_slots.end() ? nullptr : &*slot;
    }

    auto release_frame(ETH_BufferTypeDef* first) noexcept -> void
    {
        while (first != nullptr) {
            ETH_BufferTypeDef* const next{ first->next };
            const auto slot{ std::ranges::find_if(
              rx_slots, [&](const RxSlot& candidate) { return &candidate.link == first; }) };
            if (slot != rx_slots.end()) {
                slot->link = {};
                slot->allocated = false;
            }
            first = next;
        }
    }
}

extern "C" void HAL_ETH_RxAllocateCallback(std::uint8_t** buffer)
{
    if (buffer == nullptr) {
        return;
    }
    *buffer = nullptr;
    const auto slot{ std::ranges::find_if(rx_slots,
                                          [](const RxSlot& candidate) { return !candidate.allocated; }) };
    if (slot != rx_slots.end()) {
        slot->allocated = true;
        slot->link = {};
        *buffer = reinterpret_cast<std::uint8_t*>(slot->data.data());
    }
}

extern "C" void HAL_ETH_RxLinkCallback(void** start, void** end, std::uint8_t* buffer, std::uint16_t length)
{
    if (start == nullptr || end == nullptr) {
        return;
    }
    RxSlot* const slot{ find_slot(buffer) };
    if (slot == nullptr) {
        return;
    }

    slot->link.buffer = buffer;
    slot->link.len = length;
    slot->link.next = nullptr;
    if (*start == nullptr) {
        *start = &slot->link;
    }
    else {
        static_cast<ETH_BufferTypeDef*>(*end)->next = &slot->link;
    }
    *end = &slot->link;
}

namespace hal
{
    Ethernet::Ethernet(HardwareConfiguration configuration)
      : m_handle{ configuration.handle }
      , m_receiveEtherType{ configuration.configuration.receive_ether_type }
      , m_phyAddress{ configuration.configuration.phy_address }
      , m_promiscuous{ configuration.configuration.promiscuous }
    {
        for (auto& slot : rx_slots) {
            slot.link = {};
            slot.allocated = false;
        }
        if (tx_mutex_create(&m_stateMutex, state_mutex_name, TX_INHERIT) != TX_SUCCESS ||
            tx_mutex_create(&m_receiveMutex, receive_mutex_name, TX_INHERIT) != TX_SUCCESS ||
            tx_mutex_create(&m_transmitMutex, transmit_mutex_name, TX_INHERIT) != TX_SUCCESS) {
            std::terminate();
        }
    }

    Ethernet::~Ethernet()
    {
        static_cast<void>(stop());
        if (tx_mutex_delete(&m_transmitMutex) != TX_SUCCESS ||
            tx_mutex_delete(&m_receiveMutex) != TX_SUCCESS || tx_mutex_delete(&m_stateMutex) != TX_SUCCESS) {
            std::terminate();
        }
    }

    auto Ethernet::start() noexcept -> util::Result<>
    {
        ThreadXLock transmit_lock{ m_transmitMutex };
        ThreadXLock receive_lock{ m_receiveMutex };
        ThreadXLock state_lock{ m_stateMutex };
        if (m_running) {
            return {};
        }

        ETH_MACConfigTypeDef mac_configuration{};
        if (HAL_ETH_GetMACConfig(&m_handle, &mac_configuration) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        mac_configuration.Speed = ETH_SPEED_100M;
        mac_configuration.DuplexMode = ETH_FULLDUPLEX_MODE;
        mac_configuration.DropTCPIPChecksumErrorPacket = DISABLE;
        mac_configuration.TransmitFlowControl = DISABLE;
        mac_configuration.ReceiveFlowControl = DISABLE;
        if (HAL_ETH_SetMACConfig(&m_handle, &mac_configuration) != HAL_OK) {
            return error_result(std::errc::io_error);
        }

        ETH_MACFilterConfigTypeDef filter{};
        if (HAL_ETH_GetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        filter.PromiscuousMode = m_promiscuous ? ENABLE : DISABLE;
        if (HAL_ETH_SetMACFilterConfig(&m_handle, &filter) != HAL_OK) {
            return error_result(std::errc::io_error);
        }

        if (HAL_ETH_Start(&m_handle) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        m_running = true;
        return {};
    }

    auto Ethernet::stop() noexcept -> util::Result<>
    {
        ThreadXLock transmit_lock{ m_transmitMutex };
        ThreadXLock receive_lock{ m_receiveMutex };
        ThreadXLock state_lock{ m_stateMutex };
        if (!m_running) {
            return {};
        }
        if (HAL_ETH_Stop(&m_handle) != HAL_OK) {
            return error_result(std::errc::io_error);
        }
        m_running = false;
        return {};
    }

    auto Ethernet::isRunning() const noexcept -> bool
    {
        ThreadXLock state_lock{ m_stateMutex };
        return m_running;
    }

    auto Ethernet::getMacAddress() const noexcept -> MacAddress
    {
        MacAddress address{};
        if (m_handle.Init.MACAddr != nullptr) {
            std::copy_n(m_handle.Init.MACAddr, address.size(), address.begin());
        }
        return address;
    }

    auto Ethernet::getLinkInfo() const noexcept -> util::Result<LinkInfo>
    {
        std::uint32_t basic_status{};
        if (HAL_ETH_ReadPHYRegister(&m_handle, m_phyAddress, PHY_BASIC_STATUS_REGISTER, &basic_status) !=
              HAL_OK ||
            HAL_ETH_ReadPHYRegister(&m_handle, m_phyAddress, PHY_BASIC_STATUS_REGISTER, &basic_status) !=
              HAL_OK) {
            return error_result<LinkInfo>(std::errc::io_error);
        }

        LinkInfo info{ .up = (basic_status & PHY_LINK_STATUS) != 0U };
        if (!info.up) {
            return info;
        }

        std::uint32_t special_status{};
        if (HAL_ETH_ReadPHYRegister(
              &m_handle, m_phyAddress, PHY_SPECIAL_CONTROL_STATUS_REGISTER, &special_status) != HAL_OK) {
            return error_result<LinkInfo>(std::errc::io_error);
        }
        switch (special_status & PHY_SPEED_DUPLEX_MASK) {
            case PHY_10_HALF_DUPLEX:
                info.speed_mbps = 10U;
                info.duplex = Duplex::Half;
                break;
            case PHY_100_HALF_DUPLEX:
                info.speed_mbps = 100U;
                info.duplex = Duplex::Half;
                break;
            case PHY_10_FULL_DUPLEX:
                info.speed_mbps = 10U;
                info.duplex = Duplex::Full;
                break;
            case PHY_100_FULL_DUPLEX:
                info.speed_mbps = 100U;
                info.duplex = Duplex::Full;
                break;
            default:
                break;
        }
        return info;
    }

    auto Ethernet::transmit(std::span<const std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<>
    {
        if (frame.size() < ETHERNET_HEADER_SIZE || frame.size() > MAX_FRAME_SIZE || timeout.count() < 0) {
            return error_result(std::errc::invalid_argument);
        }

        ThreadXLock transmit_lock{ m_transmitMutex };
        if (!isRunning()) {
            return error_result(std::errc::not_connected);
        }

        std::ranges::copy(frame, tx_frame.begin());
        const MacAddress source{ getMacAddress() };
        for (std::size_t index{}; index < source.size(); ++index) {
            tx_frame[MAC_ADDRESS_SIZE + index] = static_cast<std::byte>(source[index]);
        }

        ETH_BufferTypeDef buffer{ .buffer = reinterpret_cast<std::uint8_t*>(tx_frame.data()),
                                  .len = static_cast<std::uint32_t>(frame.size()),
                                  .next = nullptr };
        ETH_TxPacketConfigTypeDef packet{};
        packet.Attributes = ETH_TX_PACKETS_FEATURES_SAIC | ETH_TX_PACKETS_FEATURES_CRCPAD;
        packet.Length = static_cast<std::uint32_t>(frame.size());
        packet.TxBuffer = &buffer;
        packet.SrcAddrCtrl = ETH_SRC_ADDR_REPLACE;
        packet.CRCPadCtrl = ETH_CRC_PAD_INSERT;
        switch (HAL_ETH_Transmit(&m_handle, &packet, timeout_milliseconds(timeout))) {
            case HAL_OK:
                return {};
            case HAL_TIMEOUT:
                return error_result(std::errc::timed_out);
            default:
                return error_result(std::errc::io_error);
        }
    }

    auto Ethernet::receive(std::span<std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<std::size_t>
    {
        if (frame.empty() || timeout.count() < 0) {
            return error_result<std::size_t>(std::errc::invalid_argument);
        }

        ThreadXLock receive_lock{ m_receiveMutex };
        if (!isRunning()) {
            return error_result<std::size_t>(std::errc::not_connected);
        }

        const std::uint32_t timeout_ticks{ timeout_milliseconds(timeout) };
        const std::uint32_t started_at{ HAL_GetTick() };
        while (true) {
            void* packet_pointer{};
            if (HAL_ETH_ReadData(&m_handle, &packet_pointer) == HAL_OK && packet_pointer != nullptr) {
                auto* packet{ static_cast<ETH_BufferTypeDef*>(packet_pointer) };
                std::size_t packet_size{};
                for (auto* current{ packet }; current != nullptr; current = current->next) {
                    packet_size += current->len;
                }

                if (packet_size > MAX_FRAME_SIZE || frame.size() < packet_size) {
                    release_frame(packet);
                    return error_result<std::size_t>(std::errc::no_buffer_space);
                }

                auto output{ frame.begin() };
                for (auto* current{ packet }; current != nullptr; current = current->next) {
                    output =
                      std::copy_n(reinterpret_cast<const std::byte*>(current->buffer), current->len, output);
                }
                release_frame(packet);

                const std::span<const std::byte> received_frame{ frame.data(), packet_size };
                if (!m_receiveEtherType || frameEtherType(received_frame) == m_receiveEtherType) {
                    return packet_size;
                }
            }

            if (timeout_ticks == 0U || HAL_GetTick() - started_at >= timeout_ticks) {
                return error_result<std::size_t>(std::errc::timed_out);
            }
            tx_thread_sleep(1U);
        }
    }
}
