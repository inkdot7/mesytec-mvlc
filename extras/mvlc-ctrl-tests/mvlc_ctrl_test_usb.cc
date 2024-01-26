#include "gtest/gtest.h"
#include <spdlog/spdlog.h>
#include <ftd3xx.h>
#include <mesytec-mvlc/mesytec-mvlc.h>
#include <mesytec-mvlc/mvlc_impl_usb.h>

using namespace mesytec::mvlc;

TEST(MvlcUsb, ConnectToFirstDevice)
{
    usb::Impl mvlc;

    ASSERT_EQ(mvlc.connectionType(), ConnectionType::USB);

    auto ec = mvlc.connect();
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(mvlc.isConnected());

    ec = mvlc.disconnect();
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_FALSE(mvlc.isConnected());
}

TEST(MvlcUsb, GetFtdiDriverVersions)
{
    spdlog::set_level(spdlog::level::trace);
    // TODO: move this into a setUp routine
    usb::Impl mvlc;
    auto ec = mvlc.connect();
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(mvlc.isConnected());

    auto ftHandle = mvlc.getHandle();
    ASSERT_NE(ftHandle, nullptr);

    union FtdiVersion
    {
        struct
        {
            u16 build;
            u8 minor;
            u8 major;
        };
        u32 value;
    } __attribute((packed));

    FtdiVersion driverVersion = {};

    if (auto ftSt = FT_GetDriverVersion(ftHandle, &driverVersion.value))
    {
        spdlog::error("FT_GetDriverVersion() returned {}", ftSt);
        ASSERT_EQ(ftSt, FT_OK);
    }

    spdlog::info("Ftdi Driver Version: {}.{}.{}", driverVersion.major, driverVersion.minor, driverVersion.build);

    FtdiVersion libraryVersion = {};

    if (auto ftSt = FT_GetLibraryVersion(&libraryVersion.value))
    {
        spdlog::error("FT_GetLibraryVersion() returned {}", ftSt);
        ASSERT_EQ(ftSt, FT_OK);
    }

    spdlog::info("Ftdi Library Version: {}.{}.{}", libraryVersion.major, libraryVersion.minor, libraryVersion.build);
}

TEST(MvlcUsb, ReadRegister)
{
    spdlog::set_level(spdlog::level::trace);
    // TODO: move this into a setUp routine
    usb::Impl mvlc;
    auto ec = mvlc.connect();
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(mvlc.isConnected());

    static const size_t ReadRetryMaxCount = 10;

for (size_t i=0; i<1000000; ++i)
{
    SuperCommandBuilder cmdList;
    cmdList.addReferenceWord(i); // XXX: Makes the response one word larger. 15 bytes in total now!
    cmdList.addReadLocal(registers::hardware_id);
    auto request = make_command_buffer(cmdList);

    spdlog::info("request={:#010x}", fmt::join(request, ", "));
    size_t bytesWritten = 0u;
    const size_t bytesToWrite = request.size() * sizeof(u32);
    ec = mvlc.write(Pipe::Command, reinterpret_cast<const u8 *>(request.data()), bytesToWrite, bytesWritten);

    ASSERT_FALSE(ec) << ec.message();
    ASSERT_EQ(bytesToWrite, bytesWritten);

    //std::this_thread::sleep_for(std::chrono::microseconds(100));

    // Linux: At this point the read timeout has been set to 0 at the end of
    // connect(). Reading small amounts of data immediately returns FT_TIMEOUT
    // and 0 bytes read. Starting from 1024 * 128, most times the expected
    // result of 12 bytes is returned, but not always.
    // The current APIv2 implementation doesn't run into problems because it
    // just reads in a loop.

    static const size_t responseCapacityInBytes = 4 * sizeof(u32);
    std::vector<u32> response(responseCapacityInBytes / sizeof(u32));
    const size_t responseCapacity = response.size() * sizeof(u32);
    size_t bytesRead = 0u;
    size_t retryCount = 0u;

    while (retryCount < ReadRetryMaxCount)
    {
        auto tReadStart = std::chrono::steady_clock::now();
        ec = mvlc.read(Pipe::Command, reinterpret_cast<u8 *>(response.data()), responseCapacity, bytesRead);
        auto tReadEnd = std::chrono::steady_clock::now();
        auto readElapsed = std::chrono::duration_cast<std::chrono::microseconds>(tReadEnd - tReadStart);
        spdlog::info("read(): ec={}, bytesRequested={}, bytesRead={}, read took {} µs", ec.message(), responseCapacity, bytesRead, readElapsed.count());

        if (ec != ErrorType::Timeout)
            break;

        spdlog::warn("read() timed out, retrying!");
        ++retryCount;
    }

    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(bytesRead % sizeof(u32) == 0);
    const size_t wordsRead = bytesRead / sizeof(u32);
    response.resize(wordsRead);
    spdlog::info("response={:#010x}", fmt::join(response, ", "));
    ASSERT_EQ(wordsRead, 4);
    ASSERT_EQ(response[1] & 0xffffu, i & 0xffffu);
    ASSERT_EQ(response[3], 0x5008u); // mvlc hardware id
}
}
