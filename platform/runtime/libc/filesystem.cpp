#include "libc/filesystem.hpp"

#include "hal/drivers/factory/rng.hpp"
#include "hal/drivers/factory/rtc.hpp"
#include "hal/hal.hpp"

#include <fx_api.h>
#include <tx_api.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <ranges>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(HAL_PLATFORM_STM32)
#include <dirent.h>
#include <reent.h>
#include <sys/statvfs.h>
#endif

extern "C" VOID _fx_ram_driver(FX_MEDIA* media_ptr);
extern "C" int __io_putchar(int character) __attribute__((weak));
extern "C" int __io_getchar() __attribute__((weak));

#if defined(HAL_PLATFORM_STM32)
struct runtime_filex_directory_stream
{
    struct Entry
    {
        std::array<char, runtime::filex::MAXIMUM_PATH> name{};
        std::uint8_t type{};
    };

    bool allocated{};
    long position{};
    std::array<char, runtime::filex::MAXIMUM_PATH> path{};
    std::vector<Entry> entries;
    dirent entry{};
};
#endif

namespace
{
    using namespace runtime::filex;

    constexpr ULONG SECTOR_SIZE{ 512U };
    constexpr ULONG TOTAL_SECTORS{ 64U };
    constexpr ULONG SECTORS_PER_CLUSTER{ 4U };
    constexpr std::size_t RAM_DISK_SIZE{ SECTOR_SIZE * TOTAL_SECTORS };
    constexpr std::size_t MEDIA_CACHE_SIZE{ SECTOR_SIZE * 2U };
    constexpr int FIRST_FILE_DESCRIPTOR{ 3 };

    struct FileDescriptor
    {
        FX_FILE file{};
        std::array<char, MAXIMUM_PATH> path{};
        std::uint64_t position{};
        bool allocated{};
        bool readable{};
        bool writable{};
        bool append{};
        int open_mode{ -1 };
    };

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
#if defined(HAL_PLATFORM_STM32)
    [[gnu::section(".filex_ram_disk"), gnu::used]] alignas(32)
#else
    alignas(32)
#endif
      std::array<UCHAR, RAM_DISK_SIZE> ram_disk_memory{};
    alignas(32) std::array<UCHAR, MEDIA_CACHE_SIZE> media_cache{};
    FX_MEDIA media{};
    TX_MUTEX registry_mutex{};
    std::array<FileDescriptor, MAXIMUM_OPEN_FILES> descriptors{};
#if defined(HAL_PLATFORM_STM32)
    std::array<runtime_filex_directory_stream, MAXIMUM_OPEN_DIRECTORIES> directories{};
#endif
    std::array<char, MAXIMUM_PATH> current_directory{ '/' };
    bool filesystem_initialized{};
    std::uint32_t rename_backup_sequence{};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    class RegistryGuard
    {
      public:
        RegistryGuard() noexcept
        {
            if (!filesystem_initialized) {
                m_error = ENODEV;
                return;
            }
            const UINT status{ tx_mutex_get(&registry_mutex, TX_WAIT_FOREVER) };
            m_locked = status == TX_SUCCESS;
            if (!m_locked) {
                m_error = status == TX_WAIT_ABORTED ? EINTR : EIO;
            }
        }

        ~RegistryGuard()
        {
            if (m_locked && tx_mutex_put(&registry_mutex) != TX_SUCCESS) {
                Error_Handler();
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_locked; }
        [[nodiscard]] auto error() const noexcept -> int { return m_error; }

      private:
        bool m_locked{};
        int m_error{};
    };

    [[nodiscard]] auto filex_errno(UINT status) noexcept -> int
    {
        switch (status) {
            case FX_SUCCESS:
                return 0;
            case FX_NOT_FOUND:
            case FX_NOT_OPEN:
            case FX_MEDIA_NOT_OPEN:
                return ENOENT;
            case FX_ALREADY_CREATED:
                return EEXIST;
            case FX_INVALID_NAME:
            case FX_INVALID_PATH:
            case FX_INVALID_OPTION:
            case FX_PTR_ERROR:
                return EINVAL;
            case FX_NOT_A_FILE:
                return EISDIR;
            case FX_NOT_DIRECTORY:
                return ENOTDIR;
            case FX_DIR_NOT_EMPTY:
                return ENOTEMPTY;
            case FX_NO_MORE_SPACE:
            case FX_NOT_ENOUGH_MEMORY:
                return ENOSPC;
            case FX_ACCESS_ERROR:
            case FX_WRITE_PROTECT:
                return EACCES;
            case FX_END_OF_FILE:
            case FX_NO_MORE_ENTRIES:
                return 0;
            case FX_NOT_IMPLEMENTED:
                return ENOTSUP;
            default:
                return EIO;
        }
    }

    [[nodiscard]] auto usable() noexcept -> bool
    {
#if defined(HAL_PLATFORM_STM32)
        if (__get_IPSR() != 0U) {
            errno = EPERM;
            return false;
        }
#endif
        if (!filesystem_initialized) {
            errno = ENODEV;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto normalize_path_unlocked(const char* path,
                                                char (&output)[MAXIMUM_PATH]) noexcept -> int
    {
        if (path == nullptr || path[0] == '\0') {
            return ENOENT;
        }

        std::array<char, MAXIMUM_PATH> source{};
        std::size_t source_length{};
        if (path[0] != '/' && path[0] != '\\') {
            source_length = std::strlen(current_directory.data());
            if (source_length >= source.size()) {
                return ENAMETOOLONG;
            }
            std::memcpy(source.data(), current_directory.data(), source_length);
            if (source_length > 1U && source[source_length - 1U] != '/') {
                source[source_length++] = '/';
            }
        }
        const std::size_t path_length{ std::strlen(path) };
        if (path_length >= source.size() - source_length) {
            return ENAMETOOLONG;
        }
        std::memcpy(source.data() + source_length, path, path_length + 1U);

        output[0] = '/';
        output[1] = '\0';
        std::size_t output_length{ 1U };
        const char* cursor{ source.data() };
        while (*cursor != '\0') {
            while (*cursor == '/' || *cursor == '\\') {
                ++cursor;
            }
            const char* const component{ cursor };
            while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
                ++cursor;
            }
            const std::size_t length{ static_cast<std::size_t>(cursor - component) };
            if (length == 0U || (length == 1U && component[0] == '.')) {
                continue;
            }
            if (length == 2U && component[0] == '.' && component[1] == '.') {
                if (output_length == 1U) {
                    return EACCES;
                }
                --output_length;
                while (output_length > 1U && output[output_length - 1U] != '/') {
                    --output_length;
                }
                if (output_length > 1U) {
                    --output_length;
                }
                output[output_length] = '\0';
                continue;
            }
            if (output_length != 1U) {
                if (output_length + 1U >= MAXIMUM_PATH) {
                    return ENAMETOOLONG;
                }
                output[output_length++] = '/';
            }
            if (length >= MAXIMUM_PATH - output_length) {
                return ENAMETOOLONG;
            }
            std::memcpy(output + output_length, component, length);
            output_length += length;
            output[output_length] = '\0';
        }
        return 0;
    }

    [[nodiscard]] constexpr auto fold_ascii_case(char character) noexcept -> unsigned char
    {
        const auto value{ static_cast<unsigned char>(character) };
        return value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')
                 ? static_cast<unsigned char>(value - static_cast<unsigned char>('a') +
                                              static_cast<unsigned char>('A'))
                 : value;
    }

    [[nodiscard]] auto path_has_prefix(const char* path,
                                       const char* prefix,
                                       bool include_descendants) noexcept -> bool
    {
        const std::size_t prefix_length{ std::strlen(prefix) };
        for (std::size_t index{}; index < prefix_length; ++index) {
            if (fold_ascii_case(path[index]) != fold_ascii_case(prefix[index])) {
                return false;
            }
        }
        return path[prefix_length] == '\0' || (include_descendants && path[prefix_length] == '/');
    }

    [[nodiscard]] auto paths_equal(const char* first, const char* second) noexcept -> bool
    {
        return path_has_prefix(first, second, false);
    }

    [[nodiscard]] auto migrated_path_length(const char* path,
                                            const char* old_prefix,
                                            const char* new_prefix,
                                            bool include_descendants) noexcept -> std::size_t
    {
        if (!path_has_prefix(path, old_prefix, include_descendants)) {
            return std::strlen(path);
        }
        return std::strlen(new_prefix) + std::strlen(path + std::strlen(old_prefix));
    }

    template<std::size_t Size>
    auto migrate_path(std::array<char, Size>& path,
                      const char* old_prefix,
                      const char* new_prefix,
                      bool include_descendants) noexcept -> void
    {
        if (!path_has_prefix(path.data(), old_prefix, include_descendants)) {
            return;
        }
        std::array<char, Size> migrated{};
        const std::size_t old_length{ std::strlen(old_prefix) };
        const std::size_t new_length{ std::strlen(new_prefix) };
        const char* const suffix{ path.data() + old_length };
        const std::size_t suffix_length{ std::strlen(suffix) };
        std::memcpy(migrated.data(), new_prefix, new_length);
        std::memcpy(migrated.data() + new_length, suffix, suffix_length + 1U);
        path = migrated;
    }

    [[nodiscard]] auto information_unlocked(const char* path, EntryInformation& result) noexcept -> int
    {
        char normalized[MAXIMUM_PATH]{};
        const int path_error{ normalize_path_unlocked(path, normalized) };
        if (path_error != 0) {
            return path_error;
        }
        if (std::strcmp(normalized, "/") == 0) {
            result = EntryInformation{ .attributes = FX_DIRECTORY };
            return 0;
        }

        UINT attributes{};
        ULONG size{};
        UINT year{};
        UINT month{};
        UINT day{};
        UINT hour{};
        UINT minute{};
        UINT second{};
        const UINT status{ fx_directory_information_get(&media,
                                                        normalized,
                                                        &attributes,
                                                        &size,
                                                        &year,
                                                        &month,
                                                        &day,
                                                        &hour,
                                                        &minute,
                                                        &second) };
        if (status != FX_SUCCESS) {
            return filex_errno(status);
        }
        result = EntryInformation{
            .size = size,
            .attributes = attributes,
            .year = static_cast<std::uint16_t>(year),
            .month = static_cast<std::uint8_t>(month),
            .day = static_cast<std::uint8_t>(day),
            .hour = static_cast<std::uint8_t>(hour),
            .minute = static_cast<std::uint8_t>(minute),
            .second = static_cast<std::uint8_t>(second),
        };
        return 0;
    }

    [[nodiscard]] auto rename_entry_unlocked(bool directory,
                                             const char* old_path,
                                             const char* new_path) noexcept -> int
    {
        const UINT status{ directory ? fx_directory_rename(&media,
                                                           const_cast<char*>(old_path),
                                                           const_cast<char*>(new_path))
                                     : fx_file_rename(&media,
                                                      const_cast<char*>(old_path),
                                                      const_cast<char*>(new_path)) };
        return filex_errno(status);
    }

    [[nodiscard]] auto delete_entry_unlocked(bool directory, const char* path) noexcept -> int
    {
        return filex_errno(directory ? fx_directory_delete(&media, const_cast<char*>(path))
                                     : fx_file_delete(&media, const_cast<char*>(path)));
    }

    [[nodiscard]] auto require_empty_directory_unlocked(const char* path) noexcept -> int
    {
        UINT status{ fx_directory_default_set(&media, const_cast<char*>(path)) };
        int error{ filex_errno(status) };
        if (error == 0) {
            char name[MAXIMUM_PATH]{};
            UINT attributes{};
            ULONG size{};
            status = fx_directory_first_full_entry_find(
              &media, name, &attributes, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            while (status == FX_SUCCESS &&
                   (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)) {
                status = fx_directory_next_full_entry_find(
                  &media, name, &attributes, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
            error = status == FX_NO_MORE_ENTRIES ? 0
                                                 : status == FX_SUCCESS ? ENOTEMPTY
                                                                        : filex_errno(status);
        }

        const int reset_error{
            filex_errno(fx_directory_default_set(&media, const_cast<char*>("/")))
        };
        return error != 0 ? error : reset_error;
    }

    [[nodiscard]] auto find_rename_backup_unlocked(char (&output)[MAXIMUM_PATH]) noexcept -> int
    {
        constexpr std::array<char, 16U> hexadecimal{
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
        };
        constexpr std::size_t digit_offset{ 4U };
        constexpr std::size_t digit_count{ 5U };
        constexpr std::uint32_t sequence_mask{ 0xF'FFFFU };

        // FileX has no replace-existing rename operation. Reserve a short
        // root entry so the old destination can be restored if the source
        // rename fails. The registry mutex serializes candidate allocation.
        for (std::uint32_t attempt{}; attempt < 256U; ++attempt) {
            constexpr char backup_template[]{ "/FXR00000.TMP" };
            static_assert(sizeof(backup_template) <= MAXIMUM_PATH);
            std::memcpy(output, backup_template, sizeof(backup_template));
            const std::uint32_t sequence{ rename_backup_sequence++ & sequence_mask };
            for (std::size_t digit{}; digit < digit_count; ++digit) {
                const std::size_t shift{ (digit_count - digit - 1U) * 4U };
                output[digit_offset + digit] = hexadecimal[(sequence >> shift) & 0xFU];
            }

            EntryInformation information{};
            const int error{ information_unlocked(output, information) };
            if (error == ENOENT) {
                return 0;
            }
            if (error != 0) {
                return error;
            }
        }
        return ENOSPC;
    }

    [[nodiscard]] auto switch_mode(FileDescriptor& descriptor, int requested_mode) noexcept -> int
    {
        if (descriptor.open_mode == requested_mode) {
            return 0;
        }
        if (descriptor.open_mode >= 0) {
            descriptor.position = descriptor.file.fx_file_current_file_offset;
            const UINT close_status{ fx_file_close(&descriptor.file) };
            descriptor.open_mode = -1;
            if (close_status != FX_SUCCESS) {
                return filex_errno(close_status);
            }
        }
        std::memset(&descriptor.file, 0, sizeof(descriptor.file));
        const UINT open_status{ fx_file_open(&media, &descriptor.file, descriptor.path.data(), requested_mode) };
        if (open_status != FX_SUCCESS) {
            return filex_errno(open_status);
        }
        descriptor.open_mode = requested_mode;
        const UINT seek_status{ fx_file_extended_seek(&descriptor.file, descriptor.position) };
        return filex_errno(seek_status);
    }

    [[nodiscard]] auto descriptor_for(int file) noexcept -> FileDescriptor*
    {
        const int index{ file - FIRST_FILE_DESCRIPTOR };
        if (index < 0 || std::cmp_greater_equal(index, descriptors.size()) || !descriptors[index].allocated) {
            errno = EBADF;
            return nullptr;
        }
        return &descriptors[index];
    }

    auto fill_stat(const char* path, const EntryInformation& information, struct stat* value) noexcept -> void
    {
        std::memset(value, 0, sizeof(*value));
        const bool directory{ (information.attributes & FX_DIRECTORY) != 0U };
        value->st_mode = static_cast<mode_t>((directory ? S_IFDIR : S_IFREG) |
                                            ((information.attributes & FX_READ_ONLY) != 0U ? 0444 : 0666) |
                                            (directory ? 0111 : 0));
        value->st_nlink = 1;
        std::uint32_t inode{ 2166136261U };
        for (const auto* cursor{ reinterpret_cast<const unsigned char*>(path) }; *cursor != 0U; ++cursor) {
            inode = (inode ^ fold_ascii_case(static_cast<char>(*cursor))) * 16777619U;
        }
        value->st_ino = inode;
        value->st_size = static_cast<off_t>(information.size);
        value->st_blksize = static_cast<blksize_t>(SECTOR_SIZE);
        value->st_blocks = static_cast<blkcnt_t>((information.size + SECTOR_SIZE - 1U) / SECTOR_SIZE);
        using namespace std::chrono;
        const year_month_day date{ year{ information.year },
                                   month{ information.month },
                                   day{ information.day } };
        if (date.ok()) {
            const sys_seconds timestamp{ sys_days{ date } + hours{ information.hour } +
                                         minutes{ information.minute } + seconds{ information.second } };
            value->st_mtime = static_cast<time_t>(timestamp.time_since_epoch().count());
            value->st_atime = value->st_mtime;
            value->st_ctime = value->st_mtime;
        }
    }

    auto initialize_filex_clock() noexcept -> void
    {
        const auto rtc{ hal::rtc::create() };
        if (!rtc) {
            return;
        }
        const auto timestamp{ rtc->getTime() };
        if (!timestamp || timestamp->seconds_since_epoch < 0) {
            return;
        }
        using namespace std::chrono;
        const sys_seconds point{ seconds{ timestamp->seconds_since_epoch } };
        const sys_days date{ floor<days>(point) };
        const year_month_day calendar{ date };
        const hh_mm_ss time{ point - date };
        const int year_value{ std::clamp(static_cast<int>(calendar.year()), 1980, 2107) };
        static_cast<void>(fx_system_date_set(static_cast<UINT>(year_value),
                                             static_cast<UINT>(static_cast<unsigned>(calendar.month())),
                                             static_cast<UINT>(static_cast<unsigned>(calendar.day()))));
        static_cast<void>(fx_system_time_set(static_cast<UINT>(time.hours().count()),
                                             static_cast<UINT>(time.minutes().count()),
                                             static_cast<UINT>(time.seconds().count() & ~1LL)));
    }
}

namespace runtime::filex
{
    auto initialized() noexcept -> bool { return filesystem_initialized; }

    auto normalizePath(const char* path, char (&output)[MAXIMUM_PATH]) noexcept -> int
    {
        if (!usable()) {
            return errno;
        }
        const RegistryGuard guard;
        return guard ? normalize_path_unlocked(path, output) : guard.error();
    }

    auto information(const char* path, EntryInformation& result) noexcept -> int
    {
        if (!usable()) {
            return errno;
        }
        const RegistryGuard guard;
        return guard ? information_unlocked(path, result) : guard.error();
    }

    auto availableSpace(std::uint64_t& bytes) noexcept -> int
    {
        if (!usable()) {
            return errno;
        }
        const RegistryGuard guard;
        ULONG available{};
        if (!guard) {
            bytes = 0U;
            return guard.error();
        }
        const UINT status{ fx_media_space_available(&media, &available) };
        bytes = available;
        return filex_errno(status);
    }

    auto currentPath(char (&output)[MAXIMUM_PATH]) noexcept -> int
    {
        if (!usable()) {
            return errno;
        }
        const RegistryGuard guard;
        if (!guard) {
            return guard.error();
        }
        std::memcpy(output, current_directory.data(), std::strlen(current_directory.data()) + 1U);
        return 0;
    }

    auto setCurrentPath(const char* path) noexcept -> int
    {
        if (!usable()) {
            return errno;
        }
        const RegistryGuard guard;
        if (!guard) {
            return guard.error();
        }
        char normalized[MAXIMUM_PATH]{};
        EntryInformation info{};
        int error{ normalize_path_unlocked(path, normalized) };
        if (error == 0) {
            error = information_unlocked(normalized, info);
        }
        if (error == 0 && (info.attributes & FX_DIRECTORY) == 0U) {
            error = ENOTDIR;
        }
        if (error == 0) {
            std::memcpy(current_directory.data(), normalized, std::strlen(normalized) + 1U);
        }
        return error;
    }
}

extern "C" void runtime_filex_initialize()
{
    if (filesystem_initialized) {
        return;
    }
    fx_system_initialize();
    initialize_filex_clock();
    if (tx_mutex_create(&registry_mutex, const_cast<CHAR*>("FileX descriptor registry"), TX_INHERIT) !=
        TX_SUCCESS) {
        Error_Handler();
    }
    const UINT format_status{ fx_media_format(&media,
                                              _fx_ram_driver,
                                              ram_disk_memory.data(),
                                              media_cache.data(),
                                              static_cast<ULONG>(media_cache.size()),
                                              const_cast<CHAR*>("RAM DISK"),
                                              1U,
                                              32U,
                                              0U,
                                              TOTAL_SECTORS,
                                              SECTOR_SIZE,
                                              SECTORS_PER_CLUSTER,
                                              1U,
                                              1U) };
    if (format_status != FX_SUCCESS) {
        Error_Handler();
    }
    const UINT open_status{ fx_media_open(&media,
                                          const_cast<CHAR*>("RAM DISK"),
                                          _fx_ram_driver,
                                          ram_disk_memory.data(),
                                          media_cache.data(),
                                          static_cast<ULONG>(media_cache.size())) };
    if (open_status != FX_SUCCESS) {
        Error_Handler();
    }
    current_directory.fill('\0');
    current_directory[0] = '/';
    filesystem_initialized = true;
}

#if defined(HAL_PLATFORM_STM32) || defined(HAL_PLATFORM_LINUX)
extern "C" int _open(const char* path, int flags, ...)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    char normalized[MAXIMUM_PATH]{};
    int error{ normalize_path_unlocked(path, normalized) };
    if (error != 0) {
        errno = error;
        return -1;
    }

    auto slot{ std::find_if(descriptors.begin(), descriptors.end(), [](const auto& descriptor) {
        return !descriptor.allocated;
    }) };
    if (slot == descriptors.end()) {
        errno = EMFILE;
        return -1;
    }

    EntryInformation info{};
    const int info_error{ information_unlocked(normalized, info) };
    if (info_error != 0 && info_error != ENOENT) {
        errno = info_error;
        return -1;
    }
    const bool exists{ info_error == 0 };
    if (exists && (info.attributes & FX_DIRECTORY) != 0U) {
        errno = EISDIR;
        return -1;
    }
    if (!exists && (flags & O_CREAT) == 0) {
        errno = info_error;
        return -1;
    }
    if (exists && (flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
        errno = EEXIST;
        return -1;
    }
    if (!exists) {
        const UINT create_status{ fx_file_create(&media, normalized) };
        if (create_status != FX_SUCCESS) {
            errno = filex_errno(create_status);
            return -1;
        }
        info = {};
    }

    *slot = FileDescriptor{};
    slot->allocated = true;
    slot->readable = (flags & O_ACCMODE) != O_WRONLY;
    slot->writable = (flags & O_ACCMODE) != O_RDONLY;
    slot->append = (flags & O_APPEND) != 0;
    std::memcpy(slot->path.data(), normalized, std::strlen(normalized) + 1U);
    slot->position = slot->append ? info.size : 0U;
    const int initial_mode{ slot->writable ? FX_OPEN_FOR_WRITE : FX_OPEN_FOR_READ };
    error = switch_mode(*slot, initial_mode);
    if (error == 0 && slot->writable && (flags & O_TRUNC) != 0) {
        error = filex_errno(fx_file_extended_truncate_release(&slot->file, 0U));
        slot->position = 0U;
    }
    if (error != 0) {
        if (slot->open_mode >= 0) {
            static_cast<void>(fx_file_close(&slot->file));
        }
        *slot = FileDescriptor{};
        errno = error;
        return -1;
    }
    return FIRST_FILE_DESCRIPTOR + static_cast<int>(slot - descriptors.begin());
}

extern "C" int _close(int file)
{
    if (file >= 0 && file <= 2) {
        return 0;
    }
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    if (descriptor == nullptr) {
        return -1;
    }
    const UINT status{ descriptor->open_mode >= 0 ? fx_file_close(&descriptor->file) : FX_SUCCESS };
    *descriptor = FileDescriptor{};
    if (status != FX_SUCCESS) {
        errno = filex_errno(status);
        return -1;
    }
    return 0;
}

extern "C" int _read(int file, char* buffer, int length)
{
    if (length < 0 || (buffer == nullptr && length != 0)) {
        errno = length < 0 ? EINVAL : EFAULT;
        return -1;
    }
    if (file == STDIN_FILENO) {
        if (length == 0) {
            return 0;
        }
        if (__io_getchar == nullptr) {
            errno = ENOSYS;
            return -1;
        }
        for (int index{}; index < length; ++index) {
            const int character{ __io_getchar() };
            if (character < 0) {
                errno = EIO;
                return index == 0 ? -1 : index;
            }
            buffer[index] = static_cast<char>(character);
        }
        return length;
    }
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    if (descriptor == nullptr || !descriptor->readable) {
        errno = descriptor == nullptr ? errno : EBADF;
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    const int mode_error{ switch_mode(*descriptor, FX_OPEN_FOR_READ) };
    if (mode_error != 0) {
        errno = mode_error;
        return -1;
    }
    ULONG actual{};
    const UINT status{ fx_file_read(&descriptor->file, buffer, static_cast<ULONG>(length), &actual) };
    descriptor->position = descriptor->file.fx_file_current_file_offset;
    if (status != FX_SUCCESS && status != FX_END_OF_FILE) {
        errno = filex_errno(status);
        return -1;
    }
    return static_cast<int>(actual);
}

extern "C" int _write(int file, const char* buffer, int length)
{
    if (length < 0 || (buffer == nullptr && length != 0)) {
        errno = length < 0 ? EINVAL : EFAULT;
        return -1;
    }
    if (file == STDOUT_FILENO || file == STDERR_FILENO) {
        if (length == 0) {
            return 0;
        }
        if (__io_putchar == nullptr) {
            errno = ENOSYS;
            return -1;
        }
        for (int index{}; index < length; ++index) {
            if (__io_putchar(static_cast<unsigned char>(buffer[index])) < 0) {
                errno = EIO;
                return index == 0 ? -1 : index;
            }
        }
        return length;
    }
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    if (descriptor == nullptr || !descriptor->writable) {
        errno = descriptor == nullptr ? errno : EBADF;
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    int error{ switch_mode(*descriptor, FX_OPEN_FOR_WRITE) };
    if (error == 0 && descriptor->append) {
        descriptor->position = descriptor->file.fx_file_current_file_size;
        error = filex_errno(fx_file_extended_seek(&descriptor->file, descriptor->position));
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    const UINT status{ fx_file_write(
      &descriptor->file, const_cast<char*>(buffer), static_cast<ULONG>(length)) };
    descriptor->position = descriptor->file.fx_file_current_file_offset;
    if (status != FX_SUCCESS) {
        errno = filex_errno(status);
        return -1;
    }
    return length;
}

extern "C" off_t _lseek(int file, off_t offset, int origin)
{
    if (!usable()) {
        return static_cast<off_t>(-1);
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return static_cast<off_t>(-1);
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    if (descriptor == nullptr) {
        return static_cast<off_t>(-1);
    }
    if (descriptor->open_mode < 0) {
        const int reopen_error{ switch_mode(
          *descriptor, descriptor->readable ? FX_OPEN_FOR_READ : FX_OPEN_FOR_WRITE) };
        if (reopen_error != 0) {
            errno = reopen_error;
            return static_cast<off_t>(-1);
        }
    }
    std::uint64_t base{};
    switch (origin) {
        case SEEK_SET:
            break;
        case SEEK_CUR:
            base = descriptor->position;
            break;
        case SEEK_END:
            base = descriptor->file.fx_file_current_file_size;
            break;
        default:
            errno = EINVAL;
            return static_cast<off_t>(-1);
    }
    constexpr std::uint64_t maximum_offset{ static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) };
    if (base > maximum_offset) {
        errno = EOVERFLOW;
        return static_cast<off_t>(-1);
    }

    std::uint64_t target{};
    if (offset >= 0) {
        const auto positive_offset{ static_cast<std::uint64_t>(offset) };
        if (positive_offset > maximum_offset - base) {
            errno = EOVERFLOW;
            return static_cast<off_t>(-1);
        }
        target = base + positive_offset;
    }
    else {
        const auto offset_value{ static_cast<std::int64_t>(offset) };
        const std::uint64_t magnitude{ static_cast<std::uint64_t>(-(offset_value + 1)) + 1U };
        if (magnitude > base) {
            errno = EINVAL;
            return static_cast<off_t>(-1);
        }
        target = base - magnitude;
    }

    const UINT status{ fx_file_extended_seek(&descriptor->file, static_cast<ULONG64>(target)) };
    if (status != FX_SUCCESS) {
        errno = filex_errno(status);
        return static_cast<off_t>(-1);
    }
    descriptor->position = target;
    return static_cast<off_t>(target);
}

extern "C" int _fstat(int file, struct stat* value)
{
    if (value == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (file >= 0 && file <= 2) {
        std::memset(value, 0, sizeof(*value));
        value->st_mode = S_IFCHR | 0666;
        return 0;
    }
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    EntryInformation info{};
    const int error{ descriptor != nullptr ? information_unlocked(descriptor->path.data(), info) : EBADF };
    if (error != 0) {
        errno = error;
        return -1;
    }
    if (descriptor->open_mode >= 0) {
        info.size = descriptor->file.fx_file_current_file_size;
    }
    fill_stat(descriptor->path.data(), info, value);
    return 0;
}

extern "C" int _stat(const char* path, struct stat* value)
{
    if (!usable() || value == nullptr) {
        errno = value == nullptr ? EFAULT : errno;
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    EntryInformation info{};
    const int error{ information_unlocked(path, info) };
    if (error != 0) {
        errno = error;
        return -1;
    }
    char normalized[MAXIMUM_PATH]{};
    const int normalize_error{ normalize_path_unlocked(path, normalized) };
    if (normalize_error != 0) {
        errno = normalize_error;
        return -1;
    }
    for (const auto& descriptor : descriptors) {
        if (descriptor.allocated && descriptor.open_mode >= 0 &&
            paths_equal(descriptor.path.data(), normalized)) {
            info.size = std::max<std::uint64_t>(info.size, descriptor.file.fx_file_current_file_size);
        }
    }
    fill_stat(normalized, info, value);
    return 0;
}

extern "C" int _unlink(const char* path)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    char normalized[MAXIMUM_PATH]{};
    EntryInformation info{};
    int error{ normalize_path_unlocked(path, normalized) };
    if (error == 0) {
        error = information_unlocked(normalized, info);
    }
    if (error == 0) {
        if ((info.attributes & FX_DIRECTORY) != 0U) {
            error = EISDIR;
        }
        else {
            error = filex_errno(fx_file_delete(&media, normalized));
        }
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" int _rename(const char* old_path, const char* new_path)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    char old_name[MAXIMUM_PATH]{};
    char new_name[MAXIMUM_PATH]{};
    EntryInformation info{};
    int error{ normalize_path_unlocked(old_path, old_name) };
    if (error == 0) {
        error = normalize_path_unlocked(new_path, new_name);
    }
    if (error == 0) {
        error = information_unlocked(old_name, info);
    }
    if (error == 0 && (std::strcmp(old_name, "/") == 0 || std::strcmp(new_name, "/") == 0)) {
        error = EBUSY;
    }
    if (error == 0) {
        const bool source_is_directory{ (info.attributes & FX_DIRECTORY) != 0U };
        const bool include_descendants{ source_is_directory };

        const bool spelling_changed{ std::strcmp(old_name, new_name) != 0 };
        const bool destination_is_source{ paths_equal(old_name, new_name) };
        if (source_is_directory && !destination_is_source &&
            path_has_prefix(new_name, old_name, true)) {
            // FileX accepts this move and orphans the directory tree. POSIX
            // rename must reject moving a directory below itself.
            error = EINVAL;
        }

        for (const auto& descriptor : descriptors) {
            if (error != 0) {
                break;
            }
            if (descriptor.allocated &&
                migrated_path_length(descriptor.path.data(), old_name, new_name, include_descendants) >=
                  MAXIMUM_PATH) {
                error = ENAMETOOLONG;
                break;
            }
        }
        if (error == 0 && source_is_directory &&
            migrated_path_length(current_directory.data(), old_name, new_name, true) >= MAXIMUM_PATH) {
            error = ENAMETOOLONG;
        }
#if defined(HAL_PLATFORM_STM32)
        if (error == 0 && source_is_directory) {
            for (const auto& directory : directories) {
                if (directory.allocated &&
                    migrated_path_length(directory.path.data(), old_name, new_name, true) >= MAXIMUM_PATH) {
                    error = ENAMETOOLONG;
                    break;
                }
            }
        }
#endif

        bool destination_exists{};
        bool destination_is_directory{};
        if (error == 0 && spelling_changed) {
            EntryInformation destination_info{};
            const int destination_error{ information_unlocked(new_name, destination_info) };
            if (destination_error == 0) {
                destination_exists = true;
                destination_is_directory = (destination_info.attributes & FX_DIRECTORY) != 0U;
                if (source_is_directory != destination_is_directory) {
                    error = source_is_directory ? ENOTDIR : EISDIR;
                }
                else if (!destination_is_source && source_is_directory &&
                         path_has_prefix(current_directory.data(), new_name, true)) {
                    // FileX has no anonymous directory handle with which to keep
                    // a replaced current working directory alive.
                    error = EBUSY;
                }
                else if (!destination_is_source &&
                         std::ranges::any_of(descriptors, [&](const auto& descriptor) {
                             return descriptor.allocated &&
                                    path_has_prefix(descriptor.path.data(),
                                                    new_name,
                                                    destination_is_directory);
                         })) {
                    // FileX has no anonymous file handle with which to keep a
                    // replaced destination descriptor attached to the old
                    // object. Refuse replacement instead of silently retargeting
                    // the descriptor to the source on its next mode switch.
                    error = EBUSY;
                }
            }
            else if (destination_error != ENOENT) {
                error = destination_error;
            }
        }
        if (error == 0 && destination_exists && !destination_is_source &&
            destination_is_directory) {
            error = require_empty_directory_unlocked(new_name);
        }

        if (error == 0 && spelling_changed) {
            const bool source_is_open{ std::ranges::any_of(descriptors, [&](const auto& descriptor) {
                return descriptor.allocated && descriptor.open_mode >= 0 &&
                       path_has_prefix(descriptor.path.data(), old_name, include_descendants);
            }) };
            if (source_is_open) {
                // FileX rename copies the on-media directory entry before it
                // updates open handles. Flush modified source handles first or
                // a later mode switch can reopen a stale size/cluster entry as
                // FX_FILE_CORRUPT.
                error = filex_errno(fx_media_flush(&media));
            }
        }

        char destination_backup[MAXIMUM_PATH]{};
        bool destination_was_backed_up{};
        bool source_was_renamed{};
        if (error == 0 && destination_exists && !destination_is_source) {
            error = find_rename_backup_unlocked(destination_backup);
            if (error == 0) {
                error = rename_entry_unlocked(destination_is_directory, new_name, destination_backup);
                destination_was_backed_up = error == 0;
            }
        }

        if (error == 0 && spelling_changed) {
            error = rename_entry_unlocked(source_is_directory, old_name, new_name);
            source_was_renamed = error == 0;
            if (error != 0 && destination_was_backed_up) {
                const int restore_error{
                    rename_entry_unlocked(destination_is_directory, destination_backup, new_name)
                };
                if (restore_error != 0) {
                    // Both entries still contain their original data, but an
                    // I/O failure also prevented restoring the destination's
                    // pathname. Report the restoration failure as the more
                    // actionable state of the filesystem.
                    error = restore_error;
                }
            }
        }
        if (error == 0 && destination_was_backed_up) {
            error = delete_entry_unlocked(destination_is_directory, destination_backup);
        }
        if (source_was_renamed) {
            for (auto& descriptor : descriptors) {
                if (descriptor.allocated) {
                    migrate_path(descriptor.path, old_name, new_name, include_descendants);
                }
            }
            if (source_is_directory) {
                migrate_path(current_directory, old_name, new_name, true);
#if defined(HAL_PLATFORM_STM32)
                for (auto& directory : directories) {
                    if (directory.allocated) {
                        migrate_path(directory.path, old_name, new_name, true);
                    }
                }
#endif
            }
        }
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" int _mkdir(const char* path, mode_t)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    char normalized[MAXIMUM_PATH]{};
    int error{ normalize_path_unlocked(path, normalized) };
    if (error == 0) {
        error = filex_errno(fx_directory_create(&media, normalized));
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" int _rmdir(const char* path)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    char normalized[MAXIMUM_PATH]{};
    EntryInformation info{};
    int error{ normalize_path_unlocked(path, normalized) };
    if (error == 0) {
        error = information_unlocked(normalized, info);
    }
    if (error == 0) {
        if ((info.attributes & FX_DIRECTORY) == 0U) {
            error = ENOTDIR;
        }
        else if (path_has_prefix(current_directory.data(), normalized, true)) {
            // FileX does not track a process working directory. Removing it
            // here would leave the adapter's current_directory dangling.
            error = EBUSY;
        }
        else {
            error = filex_errno(fx_directory_delete(&media, normalized));
        }
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" int _chdir(const char* path)
{
    const int error{ runtime::filex::setCurrentPath(path) };
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

extern "C" char* _getcwd(char* buffer, std::size_t size)
{
    if (buffer == nullptr || size == 0U) {
        errno = EINVAL;
        return nullptr;
    }
    char path[MAXIMUM_PATH]{};
    const int error{ runtime::filex::currentPath(path) };
    const std::size_t length{ std::strlen(path) + 1U };
    if (error != 0 || length > size) {
        errno = error != 0 ? error : ERANGE;
        return nullptr;
    }
    std::memcpy(buffer, path, length);
    return buffer;
}

extern "C" int _fsync(int file)
{
    if (!usable()) {
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    if (descriptor_for(file) == nullptr) {
        return -1;
    }
    const UINT status{ fx_media_flush(&media) };
    if (status != FX_SUCCESS) {
        errno = filex_errno(status);
        return -1;
    }
    return 0;
}

extern "C" int _ftruncate(int file, off_t length)
{
    if (!usable() || length < 0) {
        errno = length < 0 ? EINVAL : errno;
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    FileDescriptor* const descriptor{ descriptor_for(file) };
    if (descriptor == nullptr || !descriptor->writable) {
        errno = descriptor == nullptr ? errno : EBADF;
        return -1;
    }
    int error{ switch_mode(*descriptor, FX_OPEN_FOR_WRITE) };
    if (error == 0) {
        const std::uint64_t original_size{ descriptor->file.fx_file_current_file_size };
        if (std::cmp_less(length, original_size)) {
            error = filex_errno(
              fx_file_extended_truncate_release(&descriptor->file, static_cast<ULONG64>(length)));
        }
        else if (std::cmp_greater(length, original_size)) {
            error = filex_errno(fx_file_extended_seek(&descriptor->file, original_size));
            constexpr std::array<std::byte, 64U> zeroes{};
            std::uint64_t remaining{ static_cast<std::uint64_t>(length) - original_size };
            while (error == 0 && remaining > 0U) {
                const ULONG chunk{ static_cast<ULONG>(std::min<std::uint64_t>(remaining, zeroes.size())) };
                error = filex_errno(
                  fx_file_write(&descriptor->file, const_cast<std::byte*>(zeroes.data()), chunk));
                remaining -= error == 0 ? chunk : 0U;
            }
        }
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    descriptor->position = std::min<std::uint64_t>(descriptor->position, static_cast<std::uint64_t>(length));
    const int seek_error{ filex_errno(fx_file_extended_seek(&descriptor->file, descriptor->position)) };
    if (seek_error != 0) {
        errno = seek_error;
        return -1;
    }
    return 0;
}

extern "C" int _truncate(const char* path, off_t length)
{
    const int descriptor{ _open(path, O_WRONLY) };
    if (descriptor < 0) {
        return -1;
    }
    const int result{ _ftruncate(descriptor, length) };
    const int saved_errno{ errno };
    static_cast<void>(_close(descriptor));
    errno = saved_errno;
    return result;
}

extern "C" int _gettimeofday(struct timeval* value, void*)
{
    if (value == nullptr) {
        errno = EFAULT;
        return -1;
    }
    const auto rtc{ hal::rtc::create() };
    if (!rtc) {
        errno = ENODEV;
        return -1;
    }
    const auto timestamp{ rtc->getTime() };
    if (!timestamp || timestamp->nanoseconds >= hal::IRtc::NANOSECONDS_PER_SECOND ||
        !std::in_range<decltype(value->tv_sec)>(timestamp->seconds_since_epoch)) {
        errno = timestamp ? EOVERFLOW : EIO;
        return -1;
    }
    value->tv_sec = static_cast<decltype(value->tv_sec)>(timestamp->seconds_since_epoch);
    value->tv_usec = static_cast<decltype(value->tv_usec)>(timestamp->nanoseconds / 1'000U);
    return 0;
}

#if defined(HAL_PLATFORM_STM32)
namespace
{
    template<typename Result>
    auto copy_errno_to_reent(_reent* context, Result result) noexcept -> Result
    {
        if (result < 0 && context != nullptr) {
            context->_errno = errno;
        }
        return result;
    }
}

extern "C" int _open_r(_reent* context, const char* path, int flags, int mode)
{
    return copy_errno_to_reent(context, _open(path, flags, mode));
}

extern "C" int _close_r(_reent* context, int file)
{
    return copy_errno_to_reent(context, _close(file));
}

extern "C" _ssize_t _read_r(_reent* context, int file, void* buffer, std::size_t length)
{
    if (length > static_cast<std::size_t>(INT_MAX)) {
        errno = EINVAL;
        return copy_errno_to_reent(context, -1);
    }
    return copy_errno_to_reent(
      context, _read(file, static_cast<char*>(buffer), static_cast<int>(length)));
}

extern "C" _ssize_t _write_r(_reent* context, int file, const void* buffer, std::size_t length)
{
    if (length > static_cast<std::size_t>(INT_MAX)) {
        errno = EINVAL;
        return copy_errno_to_reent(context, -1);
    }
    return copy_errno_to_reent(
      context, _write(file, static_cast<const char*>(buffer), static_cast<int>(length)));
}

extern "C" _off_t _lseek_r(_reent* context, int file, _off_t offset, int origin)
{
    return static_cast<_off_t>(
      copy_errno_to_reent(context, _lseek(file, static_cast<off_t>(offset), origin)));
}

extern "C" int _fstat_r(_reent* context, int file, struct stat* value)
{
    return copy_errno_to_reent(context, _fstat(file, value));
}

extern "C" int _stat_r(_reent* context, const char* path, struct stat* value)
{
    return copy_errno_to_reent(context, _stat(path, value));
}

extern "C" int _unlink_r(_reent* context, const char* path)
{
    return copy_errno_to_reent(context, _unlink(path));
}

extern "C" int _rename_r(_reent* context, const char* old_path, const char* new_path)
{
    return copy_errno_to_reent(context, _rename(old_path, new_path));
}

extern "C" int _gettimeofday_r(_reent* context, struct timeval* value, void* timezone)
{
    return copy_errno_to_reent(context, _gettimeofday(value, timezone));
}

extern "C" int mkdir(const char* path, mode_t mode) { return _mkdir(path, mode); }
extern "C" int rmdir(const char* path) { return _rmdir(path); }
extern "C" int remove(const char* path)
{
    if (_unlink(path) == 0) {
        return 0;
    }
    return errno == EISDIR ? _rmdir(path) : -1;
}
extern "C" int chdir(const char* path) { return _chdir(path); }
extern "C" int truncate(const char* path, off_t length) { return _truncate(path, length); }
extern "C" int ftruncate(int file, off_t length) { return _ftruncate(file, length); }
extern "C" int fsync(int file) { return _fsync(file); }

extern "C" char* getcwd(char* buffer, std::size_t size)
{
    if (buffer != nullptr) {
        return _getcwd(buffer, size);
    }
    auto* const allocated{ static_cast<char*>(std::malloc(MAXIMUM_PATH)) };
    if (allocated == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    if (_getcwd(allocated, MAXIMUM_PATH) == nullptr) {
        std::free(allocated);
        return nullptr;
    }
    return allocated;
}

extern "C" long pathconf(const char*, int name)
{
    switch (name) {
#ifdef _PC_PATH_MAX
        case _PC_PATH_MAX:
            return MAXIMUM_PATH;
#endif
#ifdef _PC_NAME_MAX
        case _PC_NAME_MAX:
            return MAXIMUM_PATH - 1U;
#endif
        default:
            errno = EINVAL;
            return -1;
    }
}

extern "C" int statvfs(const char* path, struct statvfs* value)
{
    if (value == nullptr) {
        errno = EFAULT;
        return -1;
    }
    EntryInformation information{};
    const int path_error{ runtime::filex::information(path, information) };
    std::uint64_t available{};
    const int space_error{ path_error == 0 ? runtime::filex::availableSpace(available) : path_error };
    if (space_error != 0) {
        errno = space_error;
        return -1;
    }
    *value = {};
    value->f_bsize = SECTOR_SIZE;
    value->f_frsize = SECTOR_SIZE;
    value->f_blocks = TOTAL_SECTORS;
    value->f_bfree = static_cast<fsblkcnt_t>(available / SECTOR_SIZE);
    value->f_bavail = value->f_bfree;
    value->f_namemax = MAXIMUM_PATH - 1U;
    return 0;
}

extern "C" int _link(const char*, const char*)
{
    errno = ENOTSUP;
    return -1;
}

extern "C" int symlink(const char*, const char*)
{
    errno = ENOTSUP;
    return -1;
}

extern "C" ssize_t readlink(const char*, char*, std::size_t)
{
    errno = ENOTSUP;
    return -1;
}
#endif

#endif

#if defined(HAL_PLATFORM_STM32)
extern "C" int _isatty(int file) { return file >= 0 && file <= 2 ? 1 : 0; }
extern "C" int _getpid() { return 1; }
extern "C" int _kill(int, int)
{
    errno = EINVAL;
    return -1;
}
extern "C" [[noreturn]] void _exit(int)
{
    while (true) {
    }
}
extern "C" clock_t _times(struct tms*)
{
    errno = ENOSYS;
    return static_cast<clock_t>(-1);
}
extern "C" int _getentropy(void* buffer, std::size_t length)
{
    if (length == 0U) {
        return 0;
    }
    if (buffer == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (length > 256U) {
        errno = EIO;
        return -1;
    }
    if (__get_IPSR() != 0U || tx_thread_identify() == TX_NULL) {
        errno = EPERM;
        return -1;
    }
    const auto rng{ hal::rng::create() };
    if (!rng) {
        errno = ENODEV;
        return -1;
    }
    auto* output{ static_cast<std::uint8_t*>(buffer) };
    std::size_t generated{};
    while (generated < length) {
        const auto random_value{ rng->generate() };
        if (!random_value) {
            errno = EIO;
            return -1;
        }
        for (std::size_t index{}; index < sizeof(*random_value) && generated < length; ++index) {
            output[generated++] = static_cast<std::uint8_t>(*random_value >> (index * CHAR_BIT));
        }
    }
    return 0;
}

extern "C" DIR* opendir(const char* path)
{
    if (!usable()) {
        return nullptr;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return nullptr;
    }
    EntryInformation info{};
    char normalized[MAXIMUM_PATH]{};
    int error{ normalize_path_unlocked(path, normalized) };
    if (error == 0) {
        error = information_unlocked(normalized, info);
    }
    if (error == 0 && (info.attributes & FX_DIRECTORY) == 0U) {
        error = ENOTDIR;
    }
    if (error != 0) {
        errno = error;
        return nullptr;
    }
    auto slot{ std::find_if(directories.begin(), directories.end(), [](const auto& directory) {
        return !directory.allocated;
    }) };
    if (slot == directories.end()) {
        errno = EMFILE;
        return nullptr;
    }
    *slot = runtime_filex_directory_stream{};
    slot->allocated = true;
    std::memcpy(slot->path.data(), normalized, std::strlen(normalized) + 1U);
    if (fx_directory_default_set(&media, slot->path.data()) != FX_SUCCESS) {
        *slot = runtime_filex_directory_stream{};
        errno = EIO;
        return nullptr;
    }
    char name[MAXIMUM_PATH]{};
    UINT attributes{};
    ULONG size{};
    UINT status{ fx_directory_first_full_entry_find(
      &media, name, &attributes, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) };
    try {
        while (status == FX_SUCCESS) {
            if (std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0) {
                runtime_filex_directory_stream::Entry entry{};
                std::memcpy(entry.name.data(), name, std::strlen(name) + 1U);
                entry.type = (attributes & FX_DIRECTORY) != 0U ? DT_DIR : DT_REG;
                slot->entries.push_back(std::move(entry));
            }
            status = fx_directory_next_full_entry_find(
              &media, name, &attributes, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
    }
    catch (...) {
        static_cast<void>(fx_directory_default_set(&media, const_cast<CHAR*>("/")));
        *slot = runtime_filex_directory_stream{};
        errno = ENOMEM;
        return nullptr;
    }
    static_cast<void>(fx_directory_default_set(&media, const_cast<CHAR*>("/")));
    if (status != FX_NO_MORE_ENTRIES) {
        *slot = runtime_filex_directory_stream{};
        errno = filex_errno(status);
        return nullptr;
    }
    return &*slot;
}

extern "C" dirent* readdir(DIR* directory)
{
    if (!usable()) {
        return nullptr;
    }
    if (directory == nullptr) {
        errno = EBADF;
        return nullptr;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return nullptr;
    }
    if (!directory->allocated || directory->position < 0) {
        errno = EBADF;
        return nullptr;
    }
    if (std::cmp_greater_equal(directory->position, directory->entries.size())) {
        return nullptr;
    }
    const auto& snapshot{ directory->entries[static_cast<std::size_t>(directory->position)] };
    directory->entry = {};
    directory->entry.d_ino = static_cast<std::uint32_t>(directory->position + 1);
    directory->entry.d_reclen = sizeof(dirent);
    directory->entry.d_type = snapshot.type;
    std::memcpy(directory->entry.d_name, snapshot.name.data(), std::strlen(snapshot.name.data()) + 1U);
    ++directory->position;
    return &directory->entry;
}

extern "C" int closedir(DIR* directory)
{
    if (!usable()) {
        return -1;
    }
    if (directory == nullptr) {
        errno = EBADF;
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    if (!directory->allocated) {
        errno = EBADF;
        return -1;
    }
    *directory = runtime_filex_directory_stream{};
    return 0;
}

extern "C" void rewinddir(DIR* directory)
{
    if (!usable()) {
        return;
    }
    if (directory == nullptr) {
        errno = EBADF;
        return;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return;
    }
    if (!directory->allocated) {
        errno = EBADF;
        return;
    }
    directory->position = 0;
}
extern "C" long telldir(DIR* directory)
{
    if (!usable()) {
        return -1;
    }
    if (directory == nullptr) {
        errno = EBADF;
        return -1;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return -1;
    }
    if (!directory->allocated) {
        errno = EBADF;
        return -1;
    }
    return directory->position;
}
extern "C" void seekdir(DIR* directory, long position)
{
    if (!usable()) {
        return;
    }
    if (directory == nullptr) {
        errno = EBADF;
        return;
    }
    if (position < 0) {
        errno = EINVAL;
        return;
    }
    const RegistryGuard guard;
    if (!guard) {
        errno = guard.error();
        return;
    }
    if (!directory->allocated) {
        errno = EBADF;
        return;
    }
    directory->position = position;
}
extern "C" int dirfd(DIR*)
{
    errno = ENOTSUP;
    return -1;
}
#endif
