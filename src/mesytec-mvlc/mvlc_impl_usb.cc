/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "mvlc_impl_usb.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <future>
#include <iomanip>
#include <numeric>
#include <regex>

#include <ftd3xx.h>

#include "mvlc_dialog.h"
#include "mvlc_dialog_util.h"
#include "mvlc_error.h"
#include "mvlc_util.h"
#include "util/logging.h"

#define USB_WIN_USE_ASYNC 0
// TODO: remove the non-ex code paths
#define USB_WIN_USE_EX_FUNCTIONS 1 // Currently only implemented for the SYNC case.
#define USB_WIN_USE_STREAMPIPE 1

namespace std
{
    template<> struct is_error_code_enum<_FT_STATUS>: true_type {};
} // end namespace std

namespace
{
using namespace mesytec::mvlc;

static const unsigned WriteTimeout_ms = 2000;
static const unsigned ReadTimeout_ms  = 2000;

class FTErrorCategory: public std::error_category
{
    const char *name() const noexcept override
    {
        return "ftd3xx";
    }

    std::string message(int ev) const override
    {
        switch (static_cast<_FT_STATUS>(ev))
        {
            case FT_OK:                                 return "FT_OK";
            case FT_INVALID_HANDLE:                     return "FT_INVALID_HANDLE";
            case FT_DEVICE_NOT_FOUND:                   return "FT_DEVICE_NOT_FOUND";
            case FT_DEVICE_NOT_OPENED:                  return "FT_DEVICE_NOT_OPENED";
            case FT_IO_ERROR:                           return "FT_IO_ERROR";
            case FT_INSUFFICIENT_RESOURCES:             return "FT_INSUFFICIENT_RESOURCES";
            case FT_INVALID_PARAMETER: /* 6 */          return "FT_INVALID_PARAMETER";
            case FT_INVALID_BAUD_RATE:                  return "FT_INVALID_BAUD_RATE";
            case FT_DEVICE_NOT_OPENED_FOR_ERASE:        return "FT_DEVICE_NOT_OPENED_FOR_ERASE";
            case FT_DEVICE_NOT_OPENED_FOR_WRITE:        return "FT_DEVICE_NOT_OPENED_FOR_WRITE";
            case FT_FAILED_TO_WRITE_DEVICE: /* 10 */    return "FT_FAILED_TO_WRITE_DEVICE";
            case FT_EEPROM_READ_FAILED:                 return "FT_EEPROM_READ_FAILED";
            case FT_EEPROM_WRITE_FAILED:                return "FT_EEPROM_WRITE_FAILED";
            case FT_EEPROM_ERASE_FAILED:                return "FT_EEPROM_ERASE_FAILED";
            case FT_EEPROM_NOT_PRESENT:                 return "FT_EEPROM_NOT_PRESENT";
            case FT_EEPROM_NOT_PROGRAMMED:              return "FT_EEPROM_NOT_PROGRAMMED";
            case FT_INVALID_ARGS:                       return "FT_INVALID_ARGS";
            case FT_NOT_SUPPORTED:                      return "FT_NOT_SUPPORTED";
            case FT_NO_MORE_ITEMS:                      return "FT_NO_MORE_ITEMS";
            case FT_TIMEOUT: /* 19 */                   return "FT_TIMEOUT";
            case FT_OPERATION_ABORTED:                  return "FT_OPERATION_ABORTED";
            case FT_RESERVED_PIPE:                      return "FT_RESERVED_PIPE";
            case FT_INVALID_CONTROL_REQUEST_DIRECTION:  return "FT_INVALID_CONTROL_REQUEST_DIRECTION";
            case FT_INVALID_CONTROL_REQUEST_TYPE:       return "FT_INVALID_CONTROL_REQUEST_TYPE";
            case FT_IO_PENDING:                         return "FT_IO_PENDING";
            case FT_IO_INCOMPLETE:                      return "FT_IO_INCOMPLETE";
            case FT_HANDLE_EOF:                         return "FT_HANDLE_EOF";
            case FT_BUSY:                               return "FT_BUSY";
            case FT_NO_SYSTEM_RESOURCES:                return "FT_NO_SYSTEM_RESOURCES";
            case FT_DEVICE_LIST_NOT_READY:              return "FT_DEVICE_LIST_NOT_READY";
            case FT_DEVICE_NOT_CONNECTED:               return "FT_DEVICE_NOT_CONNECTED";
            case FT_INCORRECT_DEVICE_PATH:              return "FT_INCORRECT_DEVICE_PATH";

            case FT_OTHER_ERROR:                        return "FT_OTHER_ERROR";
        }

        return "unknown FT error";
    }

    std::error_condition default_error_condition(int ev) const noexcept override
    {
        using mesytec::mvlc::ErrorType;

        switch (static_cast<_FT_STATUS>(ev))
        {
            case FT_OK:
                return ErrorType::Success;

            case FT_INVALID_HANDLE:
            case FT_DEVICE_NOT_FOUND:
            case FT_DEVICE_NOT_OPENED:
            case FT_DEVICE_NOT_CONNECTED:
                return ErrorType::ConnectionError;

            case FT_IO_ERROR:
            case FT_INSUFFICIENT_RESOURCES:
            case FT_INVALID_PARAMETER:
            case FT_INVALID_BAUD_RATE:
            case FT_DEVICE_NOT_OPENED_FOR_ERASE:
            case FT_DEVICE_NOT_OPENED_FOR_WRITE:
            case FT_FAILED_TO_WRITE_DEVICE:
            case FT_EEPROM_READ_FAILED:
            case FT_EEPROM_WRITE_FAILED:
            case FT_EEPROM_ERASE_FAILED:
            case FT_EEPROM_NOT_PRESENT:
            case FT_EEPROM_NOT_PROGRAMMED:
            case FT_INVALID_ARGS:
            case FT_NOT_SUPPORTED:
            case FT_NO_MORE_ITEMS:
                return ErrorType::ConnectionError;

            case FT_TIMEOUT:
                return ErrorType::Timeout;

            case FT_OPERATION_ABORTED:
            case FT_RESERVED_PIPE:
            case FT_INVALID_CONTROL_REQUEST_DIRECTION:
            case FT_INVALID_CONTROL_REQUEST_TYPE:
            case FT_IO_PENDING:
            case FT_IO_INCOMPLETE:
            case FT_HANDLE_EOF:
            case FT_BUSY:
            case FT_NO_SYSTEM_RESOURCES:
            case FT_DEVICE_LIST_NOT_READY:
            case FT_INCORRECT_DEVICE_PATH:
            case FT_OTHER_ERROR:
                return ErrorType::ConnectionError;
        }

        assert(false);
        return {};
    }
};

const FTErrorCategory theFTErrorCategory {};

}

namespace mesytec { namespace mvlc { namespace usb
{

std::error_code make_error_code(FT_STATUS st)
{
    return { static_cast<int>(st), theFTErrorCategory };
}

}}}

namespace
{

constexpr u8 get_fifo_id(mesytec::mvlc::Pipe pipe)
{
    switch (pipe)
    {
        case mesytec::mvlc::Pipe::Command:
            return 0;
        case mesytec::mvlc::Pipe::Data:
            return 1;
    }
    return 0;
}

constexpr u8 get_endpoint(mesytec::mvlc::Pipe pipe, mesytec::mvlc::usb::EndpointDirection dir)
{
    u8 result = 0;

    switch (pipe)
    {
        case mesytec::mvlc::Pipe::Command:
            result = 0x2;
            break;

        case mesytec::mvlc::Pipe::Data:
            result = 0x3;
            break;
    }

    if (dir == mesytec::mvlc::usb::EndpointDirection::In)
        result |= 0x80;

    return result;
}

// Returns an unfiltered list of all connected FT60X devices. */
mesytec::mvlc::usb::DeviceInfoList make_device_info_list()
{
    using mesytec::mvlc::usb::DeviceInfoList;
    using mesytec::mvlc::usb::DeviceInfo;

    DeviceInfoList result;

    DWORD numDevs = 0;
    FT_STATUS st = FT_CreateDeviceInfoList(&numDevs);

    if (st == FT_OK && numDevs > 0)
    {
        auto ftInfoNodes = std::make_unique<FT_DEVICE_LIST_INFO_NODE[]>(numDevs);
        st = FT_GetDeviceInfoList(ftInfoNodes.get(), &numDevs);

        if (st == FT_OK)
        {
            result.reserve(numDevs);

            for (DWORD ftIndex = 0; ftIndex < numDevs; ftIndex++)
            {
                const auto &infoNode = ftInfoNodes[ftIndex];
                DeviceInfo di = {};
                di.index = ftIndex;
                di.serial = infoNode.SerialNumber;
                di.description = infoNode.Description;

                if (infoNode.Flags & FT_FLAGS_OPENED)
                    di.flags |= DeviceInfo::Flags::Opened;

                if (infoNode.Flags & FT_FLAGS_HISPEED)
                    di.flags |= DeviceInfo::Flags::USB2;

                if (infoNode.Flags & FT_FLAGS_SUPERSPEED)
                    di.flags |= DeviceInfo::Flags::USB3;

                di.handle = infoNode.ftHandle;

                result.emplace_back(di);
            }
        }
    }

    return result;
}

// USB specific post connect routine which tries to disable a potentially
// running DAQ. This is done to make sure the command communication is working
// properly and no readout data is clogging the USB.
// Steps:
// - Disable all triggers by writing 0 to the corresponding registers.
//   Errors are ignored except ErrorType::ConnectionError which indicate that
//   we could not open the USB device or lost the connection.
// - Read from the command pipe until no more data arrives. Again only
//   ConnectionError type errors are considered fatal.
// - Read from the data pipe until no more data arrives. These can be delayed
//   responses from writing to the trigger registers or queued up stack error
//   notifications.
// - Do a register read to check that communication is ok now.
std::error_code post_connect_cleanup(mesytec::mvlc::usb::Impl &impl)
{
    auto logger = get_logger("mvlc_usb");
    logger->debug("begin post_connect_cleanup");

    static const int DisableTriggerRetryCount = 5;
    static const size_t DataBufferSize = usb::USBStreamPipeReadSize;
    static const auto ReadDataPipeMaxWait = std::chrono::seconds(10);

    mesytec::mvlc::MVLCDialog_internal dlg(&impl);

    // Disable the triggers. There may be timeouts due to the data pipe being
    // full and no command responses arriving on the command pipe. Also
    // notification data can be stuck in the command pipe so that the responses
    // are not parsed correctly.

    // Try setting the trigger registers in a separate thread. This uses the
    // command pipe for communication.
    auto f = std::async(std::launch::async, [&dlg] () -> std::error_code
    {
        for (int try_ = 0; try_ < DisableTriggerRetryCount; try_++)
        {
            if (auto ec = disable_all_triggers_and_daq_mode(dlg))
            {
                if (ec == ErrorType::ConnectionError)
                    return ec;
            }
            else break;
        }

        return {};
    });


    // Use this thread to read the data pipe. This needs to happen so that
    // readout data doesn't clog up the data pipe bringing communication to a
    // halt.
    std::error_code ec;
    size_t totalBytesTransferred = 0u;
    size_t bytesTransferred = 0u;
    std::array<u8, DataBufferSize> buffer;
    using Clock = std::chrono::high_resolution_clock;
    auto tStart = Clock::now();

    do
    {
        bytesTransferred = 0u;
        ec = impl.read_unbuffered(Pipe::Data, buffer.data(), buffer.size(), bytesTransferred);
        //ec = impl.read(Pipe::Data, buffer.data(), buffer.size(), bytesTransferred);
        totalBytesTransferred += bytesTransferred;

        auto elapsed = Clock::now() - tStart;

        if (elapsed > ReadDataPipeMaxWait)
            break;

        if (ec == ErrorType::ConnectionError)
            break;
    } while (bytesTransferred > 0);

    ec = f.get(); // wait here for disable_all_triggers() to complete

    logger->info("end post_connect_cleanup, totalBytesTransferred={}, ec={}" ,
             totalBytesTransferred, ec.message().c_str());

    return ec;
}

std::error_code set_endpoint_timeout(void *handle, u8 ep, unsigned ms)
{
    FT_STATUS st = FT_SetPipeTimeout(handle, ep, ms);
    return mesytec::mvlc::usb::make_error_code(st);
}

} // end anon namespace

namespace mesytec
{
namespace mvlc
{
namespace usb
{

DeviceInfoList get_device_info_list(const ListOptions opts)
{
    auto result = make_device_info_list();

    if (opts == ListOptions::MVLCDevices)
    {
        // Remove if the description does not contain "MVLC"
        auto it = std::remove_if(result.begin(), result.end(), [] (const DeviceInfo &di) {
            static const std::regex reDescr("MVLC");
            return !(std::regex_search(di.description, reDescr));
        });

        result.erase(it, result.end());
    }

    return result;
}

DeviceInfo get_device_info_by_serial(const std::string &serial)
{
    auto infoList = get_device_info_list();

    auto it = std::find_if(infoList.begin(), infoList.end(),
                           [&serial] (const DeviceInfo &di) {
        return di.serial == serial;
    });

    return it != infoList.end() ? *it : DeviceInfo{};
}

std::error_code check_chip_configuration(void *handle)
{
    FT_60XCONFIGURATION conf = {};

    FT_STATUS st = FT_GetChipConfiguration(handle, &conf);

    if (st != FT_OK)
        return make_error_code(st);

    if (conf.FIFOClock != CONFIGURATION_FIFO_CLK_100
        || conf.FIFOMode != CONFIGURATION_FIFO_MODE_600
        || conf.ChannelConfig != CONFIGURATION_CHANNEL_CONFIG_2
        || !(conf.PowerAttributes & 0x40) // self powered
        || !(conf.PowerAttributes & 0x20) // remote wakup
        || conf.OptionalFeatureSupport != CONFIGURATION_OPTIONAL_FEATURE_DISABLEALL)
    {
        return std::error_code(MVLCErrorCode::USBChipConfigError);
    }

    return {};
}

//
// Impl
//
Impl::Impl()
    : m_connectMode{ConnectMode::First, {}, {}}
{
}

Impl::Impl(unsigned index)
    : m_connectMode{ConnectMode::ByIndex, index, {}}
{
}

Impl::Impl(const std::string &serial)
    : m_connectMode{ConnectMode::BySerial, 0, serial}
{
}

Impl::~Impl()
{
    disconnect();
}

std::error_code Impl::closeHandle()
{
    FT_STATUS st = FT_OK;

    if (m_handle)
    {
        st = FT_Close(m_handle);
        m_handle = nullptr;
    }

    return make_error_code(st);
}

std::error_code Impl::connect()
{
    auto logger = get_logger("mvlc_usb");
    logger->trace("begin {}", __PRETTY_FUNCTION__);

    if (isConnected())
        return make_error_code(MVLCErrorCode::IsConnected);

    FT_STATUS st = FT_OK;

    // Open the USB device. Try multiple times because with USB2 FT_Create()
    // sometimes fails the first time.
    const int MaxOpenAttempts = 3;
    DeviceInfo devInfo = {};

    for (int attempt=0; attempt<MaxOpenAttempts; ++attempt)
    {
        switch (m_connectMode.mode)
        {
            case ConnectMode::First:
                {
                    st = FT_DEVICE_NOT_FOUND;
                    auto infoList = get_device_info_list();

                    if (!infoList.empty())
                    {
                        devInfo = infoList[0];
                        st = FT_Create(reinterpret_cast<void *>(devInfo.index),
                                       FT_OPEN_BY_INDEX, &m_handle);
                    }
                }
                break;

            case ConnectMode::ByIndex:
                {
                    st = FT_DEVICE_NOT_FOUND;
                    auto infoList = get_device_info_list();

                    for (auto &info: infoList)
                    {
                        if (info.index == static_cast<int>(m_connectMode.index))
                        {
                            devInfo = info;
                            st = FT_Create(reinterpret_cast<void *>(devInfo.index),
                                           FT_OPEN_BY_INDEX, &m_handle);
                            break;
                        }
                    }
                }
                break;

            case ConnectMode::BySerial:
                {
                    st = FT_DEVICE_NOT_FOUND;

                    if ((devInfo = get_device_info_by_serial(m_connectMode.serial)))
                    {

                        st = FT_Create(reinterpret_cast<void *>(devInfo.index),
                                       FT_OPEN_BY_INDEX, &m_handle);
                    }
                }
                break;
        }

        if (st == FT_OK)
            break;
    }

    auto ec = make_error_code(st);

    logger->trace("FT_Create done: {}", ec.message());

    if (ec)
        return ec;

    m_deviceInfo = devInfo;

    if (auto ec = check_chip_configuration(m_handle))
    {
        closeHandle();
        return ec;
    }

    logger->trace("check_chip_configuration done");

    // Set actual read timeouts on the command and data pipes. Note that for
    // linux the command pipe read timeout is set to 0 later on. This initial
    // non-zero timeout is used to make the MVLCDialog operations in
    // post_connect_cleanup() work.
    for (auto pipe: { Pipe::Command, Pipe::Data})
    {
        if (auto ec = set_endpoint_timeout(
                m_handle, get_endpoint(pipe, EndpointDirection::In),
                ReadTimeout_ms))
        {
            closeHandle();
            return ec;
        }

        if (auto ec = set_endpoint_timeout(
                m_handle, get_endpoint(pipe, EndpointDirection::Out),
                WriteTimeout_ms))
        {
            closeHandle();
            return ec;
        }
    }

    logger->trace("set pipe timeouts done");

#ifdef __WIN32
    // clean up the pipes
    for (auto pipe: { Pipe::Command, Pipe::Data })
    {
        for (auto dir: { EndpointDirection::In, EndpointDirection::Out })
        {
            if (auto ec = abortPipe(pipe, dir))
            {
                closeHandle();
                return ec;
            }
        }
    }
    logger->trace("win32 pipe cleanup done");
#endif

#ifdef __WIN32
#if USB_WIN_USE_STREAMPIPE
    logger->trace("enabling streaming mode for all read pipes, size=%lu", USBStreamPipeReadSize);
    // FT_SetStreamPipe(handle, allWritePipes, allReadPipes, pipeID, streamSize)
    st = FT_SetStreamPipe(m_handle, false, true, 0, USBStreamPipeReadSize);

    if (auto ec = make_error_code(st))
    {
        logger->error("FT_SetStreamPipe failed: {}", ec.message());
        closeHandle();
        return ec;
    }

    logger->trace("win32 streampipe mode enabled");
#endif // USB_WIN_USE_STREAMPIPE
#endif // __WIN32

    logger->info("opened USB device");

    if (disableTriggersOnConnect())
    {
        if (auto ec = post_connect_cleanup(*this))
        {
            logger->warn("error from USB post connect cleanup: {}", ec.message());
            return ec;
        }

        logger->trace("post_connect_cleanup() done");
    }

#ifndef __WIN32
    // Linux only: after post_connect_cleanup() is done set the command pipes
    // read timeout to 0 which has the effect of only reading from the FTDI
    // libray buffer.
    if (auto ec = set_endpoint_timeout(m_handle, get_endpoint(Pipe::Command, EndpointDirection::In), 0))
    {
        closeHandle();
        return ec;
    }

    logger->trace("linux: CommandPipe read timeout set to 0");
#endif

    logger->trace("end {}", __PRETTY_FUNCTION__);

    return {};

}

std::error_code Impl::disconnect()
{
    if (!isConnected())
        return make_error_code(MVLCErrorCode::IsDisconnected);

    auto ec = closeHandle();

    return ec;
}

bool Impl::isConnected() const
{
    return m_handle != nullptr;
}

#ifdef __WIN32 // windows
std::error_code Impl::write(Pipe pipe, const u8 *buffer, size_t size,
                            size_t &bytesTransferred)
{
    auto logger = get_logger("mvlc_usb");

    assert(buffer);
    assert(size <= USBSingleTransferMaxBytes);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    ULONG transferred = 0; // FT API needs a ULONG*

    logger->trace("pipe={}, size={}", static_cast<unsigned>(pipe), size);

#if !USB_WIN_USE_ASYNC

#if USB_WIN_USE_EX_FUNCTIONS
    logger->trace("sync write (Ex variant)");

    FT_STATUS st = FT_WritePipeEx(
        m_handle, get_endpoint(pipe, EndpointDirection::Out),
        const_cast<u8 *>(buffer), size,
        &transferred,
        nullptr);
#else // !USB_WIN_USE_EX_FUNCTIONS
    logger->trace("sync write");
    FT_STATUS st = FT_WritePipe(
        m_handle, get_endpoint(pipe, EndpointDirection::Out),
        const_cast<u8 *>(buffer), size,
        &transferred,
        nullptr);
#endif // USB_WIN_USE_EX_FUNCTIONS

#else // USB_WIN_USE_ASYNC
    FT_STATUS st = FT_OK;
    {
        logger->trace("async write");
        OVERLAPPED vOverlapped = {};
        std::memset(&vOverlapped, 0, sizeof(vOverlapped));
        vOverlapped.hEvent = CreateEvent(nullptr, false, false, nullptr);
        //st = FT_InitializeOverlapped(m_handle, &vOverlapped);

        //qDebug("%s: vOverlapped.hEvent after call to FT_InitializeOverlapped: %p",
        //       __PRETTY_FUNCTION__, vOverlapped.hEvent);

        //if (auto ec = make_error_code(st))
        //{
        //    logger->warn("pipe=%u, FT_InitializeOverlapped failed: ec=%s",
        //             static_cast<unsigned>(pipe), ec.message().c_str());
        //    return ec;
        //}

        st = FT_WritePipe(
            m_handle, get_endpoint(pipe, EndpointDirection::Out),
            const_cast<u8 *>(buffer), size,
            &transferred,
            &vOverlapped);

        if (st == FT_IO_PENDING)
            st = FT_GetOverlappedResult(m_handle, &vOverlapped, &transferred, true);

        CloseHandle(vOverlapped.hEvent);
        //FT_ReleaseOverlapped(m_handle, &vOverlapped);
    }
#endif // USB_WIN_USE_ASYNC

    if (st != FT_OK && st != FT_IO_PENDING)
        abortPipe(pipe, EndpointDirection::Out);


    bytesTransferred = transferred;

    auto ec = make_error_code(st);

    if (ec)
    {
        logger->warn("pipe={}, wrote {} of {} bytes, result={}",
                 static_cast<unsigned>(pipe),
                 bytesTransferred, size,
                 ec.message().c_str());
    }

    return ec;
}

#else // Impl::write() linux

std::error_code Impl::write(Pipe pipe, const u8 *buffer, size_t size,
                            size_t &bytesTransferred)
{
    auto logger = get_logger("mvlc_usb");

    assert(buffer);
    assert(size <= USBSingleTransferMaxBytes);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    ULONG transferred = 0; // FT API needs a ULONG*

    FT_STATUS st = FT_WritePipeEx(m_handle, get_fifo_id(pipe),
                                  const_cast<u8 *>(buffer), size,
                                  &transferred,
                                  WriteTimeout_ms);

    bytesTransferred = transferred;

    auto ec = make_error_code(st);

    if (ec)
    {
        logger->warn("pipe={}, wrote {} of {} bytes, result={}",
                 static_cast<unsigned>(pipe),
                 bytesTransferred, size,
                 ec.message().c_str());
    }

    return ec;
}
#endif

#ifdef __WIN32 // Impl::read() windows

// Update Tue 11/05/2019:
// The note below was written before trying out overlapped I/O. This might need
// to be updated.

/* Explanation:
 * When reading from a pipe under windows any available data that was not
 * retrieved is lost instead of being returned on the next read attempt. This
 * is different than the behaviour under linux where multiple short reads can
 * be done without losing data.
 * Also the windows library does not run into a timeout if less data than
 * requested is available.
 * To work around the above issue the windows implementation uses a single read
 * buffer of size USBSingleTransferMaxBytes and only issues read requests of
 * that size. Client requests are satisfied from buffered data until the buffer
 * is empty at which point another full sized read is performed.
 */
std::error_code Impl::read(Pipe pipe, u8 *buffer, size_t size,
                           size_t &bytesTransferred)
{
    auto logger = get_logger("mvlc_usb");

    assert(buffer);
    assert(size <= USBSingleTransferMaxBytes);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    const size_t requestedSize = size;
    bytesTransferred = 0u;

    auto &readBuffer = m_readBuffers[static_cast<unsigned>(pipe)];

    // Copy from readBuffer into the dest buffer while updating local
    // variables.
    auto copy_and_update = [&buffer, &size, &bytesTransferred, &readBuffer] ()
    {
        if (size_t toCopy = std::min(readBuffer.size(), size))
        {
            memcpy(buffer, readBuffer.first, toCopy);
            buffer += toCopy;
            size -= toCopy;
            readBuffer.first += toCopy;
            bytesTransferred += toCopy;
        }
    };

    logger->trace("pipe={}, size={}, bufferSize={}",
              static_cast<unsigned>(pipe), requestedSize, readBuffer.size());

    copy_and_update();

    if (size == 0)
    {
        logger->trace("pipe={}, size={}, read request satisfied from buffer, new buffer size={}",
                  static_cast<unsigned>(pipe), requestedSize, readBuffer.size());
        return {};
    }

    // All data from the read buffer should have been consumed at this point.
    // It's time to issue an actual read request.
    assert(readBuffer.size() == 0);

    logger->trace("pipe={}, requestedSize={}, remainingSize={}, reading from MVLC...",
              static_cast<unsigned>(pipe), requestedSize, size);

    ULONG transferred = 0; // FT API wants a ULONG* parameter

#if !USB_WIN_USE_ASYNC

#if USB_WIN_USE_STREAMPIPE
    if (readBuffer.capacity() != USBStreamPipeReadSize)
        throw std::runtime_error("Read size does not equal stream pipe size");
#endif

#if USB_WIN_USE_EX_FUNCTIONS
    logger->trace("sync read (Ex variant)");

    FT_STATUS st = FT_ReadPipeEx(
        m_handle, get_endpoint(pipe, EndpointDirection::In),
        readBuffer.data.data(),
        readBuffer.capacity(),
        &transferred,
        nullptr);
#else // !USB_WIN_USE_EX_FUNCTIONS
    logger->trace("sync read");

    FT_STATUS st = FT_ReadPipe(
        m_handle, get_endpoint(pipe, EndpointDirection::In),
        readBuffer.data.data(),
        readBuffer.capacity(),
        &transferred,
        nullptr);
#endif // USB_WIN_USE_EX_FUNCTIONS

#else // USB_WIN_USE_ASYNC
    FT_STATUS st = FT_OK;
    {
        logger->trace("async read");
        OVERLAPPED vOverlapped = {};
        std::memset(&vOverlapped, 0, sizeof(vOverlapped));
        vOverlapped.hEvent = CreateEvent(nullptr, false, false, nullptr);
        //st = FT_InitializeOverlapped(m_handle, &vOverlapped);

        //if (auto ec = make_error_code(st))
        //{
        //    logger->warn("pipe=%u, FT_InitializeOverlapped failed: ec=%s",
        //             static_cast<unsigned>(pipe), ec.message().c_str());
        //    return ec;
        //}

        st = FT_ReadPipe(
            m_handle, get_endpoint(pipe, EndpointDirection::In),
            readBuffer.data.data(),
            readBuffer.capacity(),
            &transferred,
            &vOverlapped);

        if (st == FT_IO_PENDING)
            st = FT_GetOverlappedResult(m_handle, &vOverlapped, &transferred, true);

        CloseHandle(vOverlapped.hEvent);
        //FT_ReleaseOverlapped(m_handle, &vOverlapped);
    }
#endif // USB_WIN_USE_ASYNC

    if (st != FT_OK && st != FT_IO_PENDING)
        abortPipe(pipe, EndpointDirection::In);

    auto ec = make_error_code(st);

    logger->trace("pipe={}, requestedSize={}, remainingSize={}, read result: ec={}, transferred={}",
                  static_cast<unsigned>(pipe), requestedSize, size,
                  ec.message().c_str(), transferred);

    readBuffer.first = readBuffer.data.data();
    readBuffer.last  = readBuffer.first + transferred;

    copy_and_update();


    if (ec && ec != ErrorType::Timeout)
        return ec;

    if (size > 0)
    {
        logger->debug("pipe={}, requestedSize={}, remainingSize={} after read from MVLC, "
                  "returning FT_TIMEOUT (original ec={})",
                  static_cast<unsigned>(pipe), requestedSize, size,
                  ec.message().c_str());

        return make_error_code(FT_TIMEOUT);
    }

    logger->trace("pipe={}, size={}, read request satisfied after read from MVLC. new buffer size={}",
                  static_cast<unsigned>(pipe), requestedSize, readBuffer.size());

    return {};
}
#else // Impl::read() linux
std::error_code Impl::read(Pipe pipe, u8 *buffer, size_t size,
                           size_t &bytesTransferred)
{
    auto logger = get_logger("mvlc_usb");

    assert(buffer);
    assert(size <= USBSingleTransferMaxBytes);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    //logger->trace("begin read: pipe={}, size={} bytes",
    //          static_cast<unsigned>(pipe), size);

    ULONG transferred = 0; // FT API wants a ULONG* parameter

    FT_STATUS st = {};

//#if 0
    if (getDeviceInfo().flags & DeviceInfo::Flags::USB2)
    {
        // Possible fix for USB2 CommandTimeout issues with older USB chipsets:
        // explicitly specify a read timeout here.
        // -> Makes USB communication very slow.
        st = FT_ReadPipeEx(
            m_handle,
            get_fifo_id(pipe),
            buffer, size,
            &transferred,
            1);
    }
    else
//#else
    {
        st = FT_ReadPipe(
            m_handle,
            get_endpoint(pipe, EndpointDirection::In),
            buffer, size,
            &transferred,
            nullptr);
    }
//#endif

    bytesTransferred = transferred;

    auto ec = make_error_code(st);

    if (ec && ec != ErrorType::Timeout)
    {
        logger->warn("pipe={}, read {} of {} bytes, result={}",
                     static_cast<unsigned>(pipe),
                     bytesTransferred, size,
                     ec.message().c_str());
    }

    return ec;
}
#endif // Impl::read

std::error_code Impl::read_unbuffered(Pipe pipe, u8 *buffer, size_t size,
                                      size_t &bytesTransferred)
{
    auto logger = get_logger("mvlc_usb");

    assert(buffer);
    assert(static_cast<unsigned>(pipe) < PipeCount);

    if (static_cast<unsigned>(pipe) >= PipeCount)
        return make_error_code(MVLCErrorCode::InvalidPipe);

    //logger->trace("begin unbuffered read: pipe={}, size={} bytes",
    //          static_cast<unsigned>(pipe), size);

    ULONG transferred = 0; // FT API wants a ULONG* parameter
    std::error_code ec = {};

#ifdef __WIN32
#if !USB_WIN_USE_ASYNC

#if USB_WIN_USE_STREAMPIPE
    //logger->info("streampipe check");
    if (size != usb::USBSingleTransferMaxBytes)
        throw std::runtime_error("Read size does not equal stream pipe size");
#endif

#if USB_WIN_USE_EX_FUNCTIONS
    FT_STATUS st = FT_ReadPipeEx(
        m_handle, get_endpoint(pipe, EndpointDirection::In),
        buffer, size,
        &transferred,
        nullptr);
#else // !USB_WIN_USE_EX_FUNCTIONS
    FT_STATUS st = FT_ReadPipe(
        m_handle, get_endpoint(pipe, EndpointDirection::In),
        buffer, size,
        &transferred,
        nullptr);
#endif

#else // USB_WIN_USE_ASYNC
    FT_STATUS st = FT_OK;
    {
        //logger->warn("async read_unbuffered");
        OVERLAPPED vOverlapped = {};
        std::memset(&vOverlapped, 0, sizeof(vOverlapped));
        vOverlapped.hEvent = CreateEvent(nullptr, false, false, nullptr);
        //st = FT_InitializeOverlapped(m_handle, &vOverlapped);

        //if ((ec = make_error_code(st)))
        //{
        //    logger->warn("pipe=%u, FT_InitializeOverlapped failed: ec=%s",
        //             static_cast<unsigned>(pipe), ec.message().c_str());
        //    return ec;
        //}

        st = FT_ReadPipe(
            m_handle, get_endpoint(pipe, EndpointDirection::In),
            buffer, size,
            &transferred,
            &vOverlapped);

        if (st == FT_IO_PENDING)
            st = FT_GetOverlappedResult(m_handle, &vOverlapped, &transferred, true);

        CloseHandle(vOverlapped.hEvent);
        //FT_ReleaseOverlapped(m_handle, &vOverlapped);
    }

#endif // USB_WIN_USE_ASYNC

    logger->trace("result from unbuffered read: pipe={}, size={} bytes, ec={}",
              static_cast<unsigned>(pipe), size, make_error_code(st).message().c_str());

    if (st != FT_OK && st != FT_IO_PENDING)
        abortPipe(pipe, EndpointDirection::In);

#else // linux
    FT_STATUS st = FT_ReadPipe(
        m_handle, get_endpoint(pipe, EndpointDirection::In),
        buffer, size,
        &transferred,
        nullptr);
#endif

    bytesTransferred = transferred;
    ec = make_error_code(st);

    logger->trace("end unbuffered read: pipe={}, size={} bytes, transferred={} bytes, ec={}",
              static_cast<unsigned>(pipe), size, bytesTransferred, ec.message().c_str());

    return ec;
}

std::error_code Impl::abortPipe(Pipe pipe, EndpointDirection dir)
{
#ifdef __WIN32
    auto logger = get_logger("mvlc_usb");

    logger->trace(
        "FT_AbortPipe on pipe={}, dir={}",
        static_cast<unsigned>(pipe),
        static_cast<unsigned>(dir));

    auto st = FT_AbortPipe(m_handle, get_endpoint(pipe, dir));

    if (auto ec = make_error_code(st))
    {
        logger->warn(
            "FT_AbortPipe on pipe={}, dir={} returned an error: {}",
            static_cast<unsigned>(pipe),
            static_cast<unsigned>(dir),
            ec.message().c_str());
        return ec;
    }
#else // !__WIN32
    (void) pipe;
    (void) dir;
#endif // !__WIN32
    return {};
}

std::string Impl::connectionInfo() const
{
    std::string result = "speed=";

    auto devInfo = getDeviceInfo();

    if (devInfo.flags & DeviceInfo::Flags::USB2)
        result += "USB2";
    else if (devInfo.flags & DeviceInfo::Flags::USB3)
        result += "USB3";
    else
        result += "unknown";

    result += ", serial=" + devInfo.serial;

    return result;
}

} // end namespace usb
} // end namespace mvlc
} // end namespace mesytec
