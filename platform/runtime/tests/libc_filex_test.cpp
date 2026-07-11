#include "libc/filesystem.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" int _open(const char* path, int flags, ...);
extern "C" int _close(int file);
extern "C" int _read(int file, char* buffer, int length);
extern "C" int _write(int file, const char* buffer, int length);
extern "C" off_t _lseek(int file, off_t offset, int origin);
extern "C" int _fstat(int file, struct stat* value);
extern "C" int _stat(const char* path, struct stat* value);
extern "C" int _unlink(const char* path);
extern "C" int _rename(const char* old_path, const char* new_path);
extern "C" int _mkdir(const char* path, mode_t mode);
extern "C" int _rmdir(const char* path);
extern "C" int _ftruncate(int file, off_t length);

namespace
{
    std::atomic<int> console_input_calls{};
    std::atomic<int> console_input_failure_after{ -1 };
    std::atomic<int> console_output_calls{};
    std::atomic<int> console_output_failure_after{ -1 };

    class FileXTest : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite() { runtime_filex_initialize(); }

        void TearDown() override
        {
            static_cast<void>(runtime::filex::setCurrentPath("/"));
            static_cast<void>(_unlink("/renamed.bin"));
            static_cast<void>(_unlink("/mode.bin"));
            static_cast<void>(_unlink("/append.bin"));
            static_cast<void>(_unlink("/truncate.bin"));
            static_cast<void>(_unlink("/shared.bin"));
            static_cast<void>(_unlink("/seek-overflow.bin"));
            static_cast<void>(_unlink("/rename-source.bin"));
            static_cast<void>(_unlink("/rename-destination.bin"));
            static_cast<void>(_unlink("/open-rename-source.bin"));
            static_cast<void>(_unlink("/open-rename-destination.bin"));
            static_cast<void>(_unlink("/rename-destination-directory/child.bin"));
            static_cast<void>(_rmdir("/rename-source-directory"));
            static_cast<void>(_rmdir("/rename-destination-directory"));
            static_cast<void>(_unlink("/case-name.bin"));
            static_cast<void>(_unlink("/mixed-target.bin"));
            static_cast<void>(_unlink("/renamed-cwd/child/relative.bin"));
            static_cast<void>(_rmdir("/renamed-cwd/child"));
            static_cast<void>(_rmdir("/renamed-cwd"));
            static_cast<void>(_rmdir("/original-cwd/child"));
            static_cast<void>(_rmdir("/original-cwd"));
            static_cast<void>(_rmdir("/self-tree/child/grandchild"));
            static_cast<void>(_rmdir("/self-tree/child"));
            static_cast<void>(_rmdir("/self-tree"));
            static_cast<void>(_rmdir("/working-directory"));
            for (unsigned int index{}; index < 16U; ++index) {
                const std::string path{ "/entry-history/" + std::to_string(index) + ".txt" };
                static_cast<void>(_unlink(path.c_str()));
            }
            static_cast<void>(_rmdir("/entry-history"));
            static_cast<void>(_rmdir("/directory"));
            for (unsigned index{}; index < 4U; ++index) {
                const std::string path{ "/concurrent" + std::to_string(index) };
                static_cast<void>(_unlink(path.c_str()));
            }
        }
    };
}

extern "C" int __io_getchar()
{
    const int call{ console_input_calls.fetch_add(1, std::memory_order_relaxed) };
    const int failure_after{ console_input_failure_after.load(std::memory_order_relaxed) };
    return failure_after >= 0 && call >= failure_after ? -1 : 'x';
}

extern "C" int __io_putchar(int character)
{
    const int call{ console_output_calls.fetch_add(1, std::memory_order_relaxed) };
    const int failure_after{ console_output_failure_after.load(std::memory_order_relaxed) };
    return failure_after >= 0 && call >= failure_after ? -1 : character;
}

TEST_F(FileXTest, NormalizesAbsoluteAndRelativePaths)
{
    char path[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::normalizePath("/alpha//beta/./gamma", path), 0);
    EXPECT_STREQ(path, "/alpha/beta/gamma");
    EXPECT_EQ(runtime::filex::normalizePath("../../escape", path), EACCES);
}

TEST_F(FileXTest, RejectsPathsBeyondFileXLimit)
{
    std::string path(runtime::filex::MAXIMUM_PATH, 'x');
    char normalized[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::normalizePath(path.c_str(), normalized), ENAMETOOLONG);
}

TEST_F(FileXTest, NewlibDescriptorRoundTripAndMetadata)
{
    const int file{ _open("/mode.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    constexpr char payload[]{ "FileX descriptor mode switching" };
    ASSERT_EQ(_write(file, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(result, std::to_array(payload));

    struct stat metadata{};
    EXPECT_EQ(_fstat(file, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(sizeof(payload)));
    EXPECT_EQ(_close(file), 0);
    EXPECT_EQ(_stat("/mode.bin", &metadata), 0);
    EXPECT_TRUE(S_ISREG(metadata.st_mode));
}

TEST_F(FileXTest, ZeroLengthFileIoAcceptsNullBuffersForValidDescriptors)
{
    const int file{ _open("/mode.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_read(file, nullptr, 0), 0);
    EXPECT_EQ(_write(file, nullptr, 0), 0);

    errno = 0;
    EXPECT_EQ(_read(99, nullptr, 0), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(_write(99, nullptr, 0), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, ConsoleIoValidatesBuffersAndReportsPartialTransfers)
{
    console_input_calls = 0;
    console_input_failure_after = -1;
    console_output_calls = 0;
    console_output_failure_after = -1;

    EXPECT_EQ(_read(STDIN_FILENO, nullptr, 0), 0);
    EXPECT_EQ(_write(STDOUT_FILENO, nullptr, 0), 0);

    errno = 0;
    EXPECT_EQ(_read(STDIN_FILENO, nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT);
    errno = 0;
    EXPECT_EQ(_write(STDERR_FILENO, nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT);

    char input[4]{};
    console_input_calls = 0;
    console_input_failure_after = 2;
    EXPECT_EQ(_read(STDIN_FILENO, input, sizeof(input)), 2);
    EXPECT_EQ(std::string_view(input, 2), "xx");
    console_input_calls = 0;
    console_input_failure_after = 0;
    errno = 0;
    EXPECT_EQ(_read(STDIN_FILENO, input, sizeof(input)), -1);
    EXPECT_EQ(errno, EIO);

    constexpr char output[]{ "abcd" };
    console_output_calls = 0;
    console_output_failure_after = 2;
    EXPECT_EQ(_write(STDOUT_FILENO, output, 4), 2);
    console_output_calls = 0;
    console_output_failure_after = 0;
    errno = 0;
    EXPECT_EQ(_write(STDOUT_FILENO, output, 4), -1);
    EXPECT_EQ(errno, EIO);

    console_input_failure_after = -1;
    console_output_failure_after = -1;
}

TEST_F(FileXTest, SeekRejectsOffsetsThatCannotBeRepresentedWithoutOverflow)
{
    const int file{ _open("/seek-overflow.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_lseek(file, std::numeric_limits<off_t>::max(), SEEK_SET),
              std::numeric_limits<off_t>::max());

    errno = 0;
    EXPECT_EQ(_lseek(file, 1, SEEK_CUR), static_cast<off_t>(-1));
    EXPECT_EQ(errno, EOVERFLOW);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, RenameReplacesDestinationAndPreservesOpenSourceDescriptor)
{
    int destination{ _open("/rename-destination.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(destination, 3);
    ASSERT_EQ(_write(destination, "old", 3), 3);
    ASSERT_EQ(_close(destination), 0);

    const int source{ _open("/rename-source.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(source, 3);
    constexpr char payload[]{ "replacement" };
    ASSERT_EQ(_write(source, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(_rename("/rename-source.bin", "/rename-destination.bin"), 0);

    struct stat metadata{};
    EXPECT_EQ(_fstat(source, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(sizeof(payload)));
    ASSERT_EQ(_lseek(source, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    errno = 0;
    const int read_result{ _read(source, result.data(), result.size()) };
    EXPECT_EQ(read_result, static_cast<int>(result.size())) << std::strerror(errno);
    EXPECT_EQ(result, std::to_array(payload));
    EXPECT_EQ(_close(source), 0);

    errno = 0;
    EXPECT_EQ(_stat("/rename-source.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/rename-destination.bin", &metadata), 0);
}

TEST_F(FileXTest, RenameDoesNotRetargetAnOpenDestinationDescriptor)
{
    const int source{ _open("/open-rename-source.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(source, 3);
    ASSERT_EQ(_write(source, "source", 6), 6);
    ASSERT_EQ(_close(source), 0);

    const int destination{
        _open("/open-rename-destination.bin", O_CREAT | O_RDWR | O_TRUNC, 0666)
    };
    ASSERT_GE(destination, 3);
    ASSERT_EQ(_write(destination, "destination", 11), 11);

    errno = 0;
    EXPECT_EQ(_rename("/open-rename-source.bin", "/open-rename-destination.bin"), -1);
    EXPECT_EQ(errno, EBUSY);

    ASSERT_EQ(_lseek(destination, 0, SEEK_SET), 0);
    std::array<char, 11> destination_contents{};
    EXPECT_EQ(_read(destination, destination_contents.data(), destination_contents.size()),
              static_cast<int>(destination_contents.size()));
    EXPECT_EQ(std::string_view(destination_contents.data(), destination_contents.size()), "destination");
    EXPECT_EQ(_close(destination), 0);

    struct stat metadata{};
    EXPECT_EQ(_stat("/open-rename-source.bin", &metadata), 0);
    EXPECT_EQ(_stat("/open-rename-destination.bin", &metadata), 0);
}

TEST_F(FileXTest, RenamePreservesANonemptyDestinationDirectoryOnFailure)
{
    ASSERT_EQ(_mkdir("/rename-source-directory", 0777), 0);
    ASSERT_EQ(_mkdir("/rename-destination-directory", 0777), 0);
    const int child{
        _open("/rename-destination-directory/child.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666)
    };
    ASSERT_GE(child, 3);
    ASSERT_EQ(_close(child), 0);

    errno = 0;
    EXPECT_EQ(_rename("/rename-source-directory", "/rename-destination-directory"), -1);
    EXPECT_EQ(errno, ENOTEMPTY);

    struct stat metadata{};
    EXPECT_EQ(_stat("/rename-source-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
    EXPECT_EQ(_stat("/rename-destination-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
    EXPECT_EQ(_stat("/rename-destination-directory/child.bin", &metadata), 0);

    ASSERT_EQ(_unlink("/rename-destination-directory/child.bin"), 0);
    EXPECT_EQ(_rename("/rename-source-directory", "/rename-destination-directory"), 0);
    errno = 0;
    EXPECT_EQ(_stat("/rename-source-directory", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/rename-destination-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
}

TEST_F(FileXTest, RenameMigratesCurrentDirectoryAndRelativePaths)
{
    ASSERT_EQ(_mkdir("/Original-Cwd", 0777), 0);
    ASSERT_EQ(_mkdir("/Original-Cwd/child", 0777), 0);
    ASSERT_EQ(runtime::filex::setCurrentPath("/original-cwd/child"), 0);
    ASSERT_EQ(_rename("/ORIGINAL-CWD", "/RENAMED-CWD"), 0);

    char current[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::currentPath(current), 0);
    EXPECT_STREQ(current, "/RENAMED-CWD/child");

    const int file{ _open("relative.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_close(file), 0);
    struct stat metadata{};
    EXPECT_EQ(_stat("/renamed-cwd/child/relative.bin", &metadata), 0);
}

TEST_F(FileXTest, CaseInsensitiveAliasesPreserveIdentityAndOpenDescriptors)
{
    const int file{ _open("/Case-Name.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    constexpr char payload[]{ "case-insensitive FileX identity" };
    ASSERT_EQ(_write(file, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));

    ASSERT_EQ(_rename("/case-name.bin", "/CASE-NAME.BIN"), 0);
    struct stat upper_metadata{};
    struct stat lower_metadata{};
    ASSERT_EQ(_stat("/CASE-NAME.BIN", &upper_metadata), 0);
    ASSERT_EQ(_stat("/case-name.bin", &lower_metadata), 0);
    EXPECT_EQ(upper_metadata.st_ino, lower_metadata.st_ino);

    ASSERT_EQ(_rename("/case-name.bin", "/Mixed-Target.bin"), 0);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(result, std::to_array(payload));
    EXPECT_EQ(_close(file), 0);

    struct stat metadata{};
    errno = 0;
    EXPECT_EQ(_stat("/case-name.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/mixed-target.bin", &metadata), 0);
}

TEST_F(FileXTest, RejectsDirectoryMovesBelowItselfAndRemovalOfCurrentDirectory)
{
    ASSERT_EQ(_mkdir("/self-tree", 0777), 0);
    ASSERT_EQ(_mkdir("/self-tree/child", 0777), 0);
    errno = 0;
    EXPECT_EQ(_rename("/self-tree", "/self-tree/child/grandchild"), -1);
    EXPECT_EQ(errno, EINVAL);

    struct stat metadata{};
    EXPECT_EQ(_stat("/self-tree", &metadata), 0);
    EXPECT_EQ(_stat("/self-tree/child", &metadata), 0);

    ASSERT_EQ(_mkdir("/working-directory", 0777), 0);
    ASSERT_EQ(runtime::filex::setCurrentPath("/working-directory"), 0);
    errno = 0;
    EXPECT_EQ(_rmdir("/WORKING-DIRECTORY"), -1);
    EXPECT_EQ(errno, EBUSY);
    ASSERT_EQ(runtime::filex::setCurrentPath("/"), 0);
    EXPECT_EQ(_rmdir("/working-directory"), 0);
}

TEST_F(FileXTest, DeletesDirectoryWithManyFormerEntriesOnSmallFatVolume)
{
    ASSERT_EQ(_mkdir("/entry-history", 0777), 0);
    for (unsigned int index{}; index < 16U; ++index) {
        const std::string path{ "/entry-history/" + std::to_string(index) + ".txt" };
        const int file{ _open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666) };
        ASSERT_GE(file, 3);
        ASSERT_EQ(_close(file), 0);
        ASSERT_EQ(_unlink(path.c_str()), 0);
    }
    EXPECT_EQ(_rmdir("/entry-history"), 0);
}

TEST_F(FileXTest, AppendAndTruncateAreDeterministic)
{
    int file{ _open("/append.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "one", 3), 3);
    ASSERT_EQ(_close(file), 0);

    file = _open("/append.bin", O_WRONLY | O_APPEND);
    ASSERT_GE(file, 3);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    ASSERT_EQ(_write(file, "two", 3), 3);
    ASSERT_EQ(_ftruncate(file, 4), 0);
    ASSERT_EQ(_close(file), 0);

    file = _open("/append.bin", O_RDONLY);
    ASSERT_GE(file, 3);
    std::array<char, 8> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), 4);
    EXPECT_EQ(std::string_view(result.data(), 4), "onet");
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, ExtendingAFileZeroFillsTheNewRange)
{
    int file{ _open("/truncate.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "abc", 3), 3);
    ASSERT_EQ(_ftruncate(file, 8), 0);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<unsigned char, 8> result{};
    ASSERT_EQ(_read(file, reinterpret_cast<char*>(result.data()), result.size()),
              static_cast<int>(result.size()));
    EXPECT_EQ(result[0], 'a');
    EXPECT_EQ(result[1], 'b');
    EXPECT_EQ(result[2], 'c');
    EXPECT_EQ(result[3], 0U);
    EXPECT_EQ(result[7], 0U);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, DescriptorTableHasExactlySixteenFileSlots)
{
    std::vector<int> files;
    for (std::size_t index{}; index < runtime::filex::MAXIMUM_OPEN_FILES; ++index) {
        const std::string path{ "/slot" + std::to_string(index) };
        const int file{ _open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666) };
        ASSERT_GE(file, 3);
        files.push_back(file);
    }
    errno = 0;
    EXPECT_EQ(_open("/overflow", O_CREAT | O_RDWR, 0666), -1);
    EXPECT_EQ(errno, EMFILE);
    for (int file : files) {
        EXPECT_EQ(_close(file), 0);
    }
    for (std::size_t index{}; index < files.size(); ++index) {
        const std::string path{ "/slot" + std::to_string(index) };
        EXPECT_EQ(_unlink(path.c_str()), 0);
    }
    errno = 0;
    EXPECT_EQ(_unlink("/overflow"), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileXTest, RenameDirectoriesAndInvalidHandles)
{
    EXPECT_EQ(_mkdir("/directory", 0777), 0);
    errno = 0;
    EXPECT_EQ(_unlink("/directory"), -1);
    EXPECT_EQ(errno, EISDIR);
    EXPECT_EQ(_rmdir("/directory"), 0);
    int file{ _open("/mode.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_close(file), 0);
    errno = 0;
    EXPECT_EQ(_rmdir("/mode.bin"), -1);
    EXPECT_EQ(errno, ENOTDIR);
    EXPECT_EQ(_rename("/mode.bin", "/renamed.bin"), 0);
    errno = 0;
    char unused{};
    EXPECT_EQ(_read(file, &unused, 0), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(FileXTest, SerializesSharedAndDistinctThreadAccess)
{
    const int shared{ _open("/shared.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(shared, 3);
    constexpr std::array<char, 64> payload{};
    std::array<std::thread, 4> threads;
    for (unsigned index{}; index < threads.size(); ++index) {
        threads[index] = std::thread{ [shared, index, &payload] {
            EXPECT_EQ(_write(shared, payload.data(), payload.size()), static_cast<int>(payload.size()));
            const std::string path{ "/concurrent" + std::to_string(index) };
            const int file{ _open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666) };
            ASSERT_GE(file, 3);
            EXPECT_EQ(_write(file, payload.data(), payload.size()), static_cast<int>(payload.size()));
            EXPECT_EQ(_close(file), 0);
        } };
    }
    for (auto& thread : threads) {
        thread.join();
    }
    struct stat metadata{};
    EXPECT_EQ(_fstat(shared, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(payload.size() * threads.size()));
    EXPECT_EQ(_close(shared), 0);
}
