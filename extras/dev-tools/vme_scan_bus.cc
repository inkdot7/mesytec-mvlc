#include <mesytec-mvlc/mesytec-mvlc.h>
#include <lyra/lyra.hpp>

using std::cout;
using std::cerr;
using std::endl;

using namespace mesytec::mvlc;

static const u32 FirmwareRegister = 0x600eu;
static const u32 HardwareIdRegister = 0x6008u;

// Full 16 bit values of the hardware id register (0x6008).
struct HardwareIds
{
    static const u16 MADC_32 = 0x5002;
    static const u16 MQDC_32 = 0x5003;
    static const u16 MTDC_32 = 0x5004;
    static const u16 MDPP_16 = 0x5005;
    // The VMMRs use the exact same software, so the hardware ids are equal.
    // VMMR-8 is a VMMR-16 with the 8 high busses not yielding data.
    static const u16 VMMR_8  = 0x5006;
    static const u16 VMMR_16 = 0x5006;
    static const u16 MDPP_32 = 0x5007;
};

// Firmware type is encoded in the highest nibble of the firmware register
// (0x600e). The lower nibbles contain the firmware revision. Valid for both
// MDPP-16 and MDPP-32 but not all packages exist for the MDPP-32.
enum MDPP_FirmwareType
{
    PADC = 0,
    SCP  = 1,
    RCP  = 2,
    QDC  = 3,
};

struct MDPP_FirmwareInfo
{
    static const u32 Mask = 0xf000u;
    static const u32 Shift = 12u;
};

inline unsigned mdpp_firmware_type_from_register(u16 fwReg)
{
    return (fwReg & MDPP_FirmwareInfo::Mask) >> MDPP_FirmwareInfo::Shift;
}

inline std::string hardware_id_to_module_name(u16 hwid)
{
    switch (hwid)
    {
        case HardwareIds::MADC_32: return "MADC_32";
        case HardwareIds::MQDC_32: return "MQDC_32";
        case HardwareIds::MTDC_32: return "MTDC_32";
        case HardwareIds::MDPP_16: return "MDPP_16";
        case HardwareIds::VMMR_8:  return "VMMR_8/16";
        case HardwareIds::MDPP_32: return "MDPP_32";
    }
    return {};
}

inline std::string mdpp_firmware_name(int fwType)
{
    switch (fwType)
    {
        case MDPP_FirmwareType::PADC: return "PADC";
        case MDPP_FirmwareType::SCP:  return "SCP";
        case MDPP_FirmwareType::RCP:  return "RCP";
        case MDPP_FirmwareType::QDC:  return "QDC";
    }
    return {};
}

inline bool is_mdpp(u16 hwId)
{
    return hwId == HardwareIds::MDPP_16 || hwId == HardwareIds::MDPP_32;
}

// Scans the uppper 64k addresses for mesytec modules. Returns a list of
// candidate addresses.
std::vector<u32> scan_vme_bus_for_candidates(MVLC &mvlc)
{
    std::vector<u32> response;
    std::vector<u32> result;

    // Note: 0xffff itself is never checked as that is taken by the MVLC itself.
    const u32 baseMax = 0xffffu;
    u32 base = 0;

    do
    {
        StackCommandBuilder sb;
        sb.addWriteMarker(0x13370001u);
        u32 baseStart = base;

        while (get_encoded_stack_size(sb) < MirrorTransactionMaxContentsWords / 2 - 2
                && base < baseMax)
        {
            u32 hwReg = (base << 16) + 0x6008;
            sb.addVMERead(hwReg, vme_amods::A32, VMEDataWidth::D16);
            ++base;
        }

        spdlog::debug("Executing stack. size={}, baseStart=0x{:08x}, baseEnd=0x{:08x}, #addresses={}",
            get_encoded_stack_size(sb), baseStart, base, base - baseStart);

        if (auto ec = mvlc.stackTransaction(sb, response))
            throw std::system_error(ec);

        spdlog::debug("Stack result for baseStart=0x{:08x}, baseEnd=0x{:#08x}, response.size()={}, response={:#010x}\n",
            baseStart, base, response.size(), fmt::join(response, ", "));

        // +2 to skip over 0xF3 and the marker
        for (auto it = std::begin(response) + 2; it < std::end(response); ++it)
        {
            auto index = std::distance(std::begin(response) + 2, it);
            auto value = *it;
            if ((value & 0xffffff00) != 0xffffff00)
            {
                u32 addr = (baseStart + index) << 16;
                result.push_back(addr);
                spdlog::debug("index={}, value=0x{:08x}, addr={:#010x}", index, value, addr);
            }
        }

        response.clear();

    } while (base < baseMax);

    return result;
}

struct VMEModuleInfo
{
    u32 hwId;
    u32 fwId;

    std::string moduleTypeName() const
    {
        return hardware_id_to_module_name(hwId);
    }

    std::string mdppFirmwareTypeName() const
    {
        if (is_mdpp(hwId))
            return mdpp_firmware_name(mdpp_firmware_type_from_register(fwId));
        return {};
    }
};

int main(int argc, char *argv[])
{
    bool opt_showHelp = false;
    bool opt_logDebug = false;
    bool opt_logTrace = false;
    std::string opt_mvlcEthHost;
    bool opt_mvlcUseFirstUSBDevice = true;

    auto cli
        = lyra::help(opt_showHelp)

        | lyra::opt(opt_mvlcEthHost, "hostname")
            ["--mvlc-eth"] ("mvlc ethernet hostname")

        | lyra::opt(opt_mvlcUseFirstUSBDevice)
            ["--mvlc-usb"] ("connect to the first mvlc usb device")

        | lyra::opt(opt_logDebug)["--debug"]("enable debug logging")

        | lyra::opt(opt_logTrace)["--trace"]("enable trace logging")
        ;

    auto cliParseResult = cli.parse({ argc, argv });

    if (!cliParseResult)
    {
        cerr << "Error parsing command line arguments: " << cliParseResult.errorMessage() << endl;
        return 1;
    }

    if (opt_showHelp)
    {
        cout << cli << endl;
        return 0;
    }

    // logging setup
    if (opt_logDebug)
        set_global_log_level(spdlog::level::debug);

    if (opt_logTrace)
        set_global_log_level(spdlog::level::trace);

    try
    {
        MVLC mvlc;

        if (!opt_mvlcEthHost.empty())
            mvlc = make_mvlc_eth(opt_mvlcEthHost);
        else
            mvlc = make_mvlc_usb();

        if (auto ec = mvlc.connect())
        {
            cerr << "Error connecting to MVLC: " << ec.message() << endl;
            return 1;
        }

        if (auto candidates = scan_vme_bus_for_candidates(mvlc);
            !candidates.empty())
        {
            spdlog::info("Found module candidate addresses: {:#010x}", fmt::join(candidates, ", "));

            for (auto addr: candidates)
            {
                VMEModuleInfo moduleInfo{};
                if (auto ec = mvlc.vmeRead(addr + FirmwareRegister, moduleInfo.fwId, vme_amods::A32, VMEDataWidth::D16))
                {
                    spdlog::info("Error checking address {#:010x}: {}", addr, ec.message());
                    continue;
                }

                if (auto ec = mvlc.vmeRead(addr + HardwareIdRegister, moduleInfo.hwId, vme_amods::A32, VMEDataWidth::D16))
                {
                    spdlog::info("Error checking address {#:010x}: {}", addr, ec.message());
                    continue;
                }

                spdlog::info("Found module at address {:#010x}: hwId={:#06x}, fwId={:#06x}, type={}, mdpp firmware type={}",
                    addr, moduleInfo.hwId, moduleInfo.fwId, moduleInfo.moduleTypeName(), moduleInfo.mdppFirmwareTypeName());
            }
        }
        else
            spdlog::info("scan bus did not find any mesytec VME modules");
    }
    catch (const std::exception &e)
    {
        cerr << "caught an exception: " << e.what() << endl;
        return 1;
    }
}