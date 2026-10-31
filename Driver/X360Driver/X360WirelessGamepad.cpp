//
//  X360WirelessGamepad.cpp
//  X360Driver
//

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSCollections.h>
#include <HIDDriverKit/IOHIDDeviceKeys.h>

#include "X360WirelessGamepad.h"
#include "X360WirelessReceiver.h"
#include "X360Protocol.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "X360Driver gamepad: " fmt, ##__VA_ARGS__)

/// Property (set per slot in Info.plist) telling the gamepad which receiver slot it represents.
#define kX360SlotKey          "X360Slot"
/// Same property name 360Controller used, so existing tools can read it.
#define kX360BatteryLevelKey  "BatteryLevel"

struct X360WirelessGamepad_IVars {
    X360WirelessReceiver     *receiver;     // Our provider; outlives us, not retained.
    IOBufferMemoryDescriptor *report;
    uint32_t                  slot;
};

// MARK: - Lifecycle

bool X360WirelessGamepad::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(X360WirelessGamepad_IVars, 1);
    return ivars != nullptr;
}

void X360WirelessGamepad::free()
{
    if (ivars != nullptr) {
        OSSafeReleaseNULL(ivars->report);
    }
    IOSafeDeleteNULL(ivars, X360WirelessGamepad_IVars, 1);
    super::free();
}

kern_return_t IMPL(X360WirelessGamepad, Start)
{
    // Resolve receiver and slot first: super's Start asks for the device description.
    ivars->receiver = OSDynamicCast(X360WirelessReceiver, provider);
    if (ivars->receiver == nullptr) {
        Log("provider is not the receiver");
        return kIOReturnNoDevice;
    }
    OSDictionary *properties = nullptr;
    if (CopyProperties(&properties) == kIOReturnSuccess && properties != nullptr) {
        OSNumber *slot = OSDynamicCast(OSNumber, OSDictionaryGetValue(properties, kX360SlotKey));
        if (slot != nullptr) {
            ivars->slot = slot->unsigned32BitValue();
        }
        properties->release();
    }

    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, X360_HID_REPORT_SIZE, 0, &ivars->report);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super Start failed for slot %u: 0x%x", ivars->slot, ret);
        return ret;
    }
    Log("started for slot %u", ivars->slot);
    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(X360WirelessGamepad, Stop)
{
    Log("stopping slot %u", ivars->slot);
    if (ivars->receiver != nullptr) {
        ivars->receiver->GamepadDidStop(ivars->slot, this);
        ivars->receiver = nullptr;
    }
    return Stop(provider, SUPERDISPATCH);
}

bool X360WirelessGamepad::handleStart(IOService *provider)
{
    return super::handleStart(provider);
}

// MARK: - HID description

static void SetNumber(OSDictionary *dictionary, const char *key, uint64_t value)
{
    OSNumber *number = OSNumber::withNumber(value, 32);
    if (number != nullptr) {
        OSDictionarySetValue(dictionary, key, number);
        number->release();
    }
}

static void SetString(OSDictionary *dictionary, const char *key, const char *value)
{
    OSString *string = OSString::withCString(value);
    if (string != nullptr) {
        OSDictionarySetValue(dictionary, key, string);
        string->release();
    }
}

OSDictionary *X360WirelessGamepad::newDeviceDescription()
{
    OSDictionary *description = OSDictionary::withCapacity(12);
    if (description == nullptr) {
        return nullptr;
    }
    uint32_t location = ivars->receiver ? ivars->receiver->GetLocationID() : 0;
    char serial[] = "X360-SLOT-0";
    serial[sizeof(serial) - 2] = (char)('1' + ivars->slot);

    // Identity of the pads the 360Controller kext published, so SDL/game mappings apply.
    SetNumber(description, kIOHIDVendorIDKey, X360_VENDOR_MICROSOFT);
    SetNumber(description, kIOHIDProductIDKey, X360_PRODUCT_WIRED_CONTROLLER);
    SetNumber(description, kIOHIDVersionNumberKey, 0x0114);
    SetString(description, kIOHIDTransportKey, "Wireless");
    SetString(description, kIOHIDManufacturerKey, "Microsoft");
    SetString(description, kIOHIDProductKey, "Xbox 360 Wireless Controller");
    SetString(description, kIOHIDSerialNumberKey, serial);
    // Distinct location per slot: some games use it to tell pads apart.
    SetNumber(description, kIOHIDLocationIDKey, location + ivars->slot + 1);
    SetNumber(description, kIOHIDPrimaryUsagePageKey, 0x01);   // Generic Desktop
    SetNumber(description, kIOHIDPrimaryUsageKey, 0x05);       // Game Pad
    SetNumber(description, kIOHIDCountryCodeKey, 0);
    SetNumber(description, kIOHIDReportIntervalKey, 8000);     // µs
    return description;
}

OSData *X360WirelessGamepad::newReportDescriptor()
{
    return OSData::withBytes(X360HIDReportDescriptor, X360HIDReportDescriptorSize);
}

// MARK: - Input

void X360WirelessGamepad::PostPadState(const struct X360PadState *state, uint64_t timestamp)
{
    IOAddressSegment range = {};
    if (ivars->report == nullptr || ivars->report->GetAddressRange(&range) != kIOReturnSuccess) {
        return;
    }
    size_t length = X360BuildHIDReport(state, reinterpret_cast<uint8_t *>(range.address), range.length);
    if (length == 0) {
        return;
    }
    ivars->report->SetLength(length);
    kern_return_t ret = handleReport(timestamp, ivars->report, (uint32_t)length, kIOHIDReportTypeInput, 0);
    if (ret != kIOReturnSuccess) {
        Log("handleReport failed for slot %u: 0x%x", ivars->slot, ret);
    }
}

void X360WirelessGamepad::SetBatteryLevel(uint8_t level)
{
    OSString *key = OSString::withCString(kX360BatteryLevelKey);
    OSNumber *value = OSNumber::withNumber(level, 8);
    if (key != nullptr && value != nullptr) {
        setProperty(key, value);
    }
    OSSafeReleaseNULL(key);
    OSSafeReleaseNULL(value);
}

// MARK: - Output

kern_return_t X360WirelessGamepad::setReport(IOMemoryDescriptor *report,
                                             IOHIDReportType reportType,
                                             IOOptionBits options,
                                             uint32_t completionTimeout,
                                             OSAction *action)
{
    if (report == nullptr || ivars->receiver == nullptr) {
        return kIOReturnBadArgument;
    }

    uint64_t length = 0;
    IOMemoryMap *map = nullptr;
    kern_return_t ret = report->GetLength(&length);
    if (ret == kIOReturnSuccess) {
        ret = report->CreateMapping(0, 0, 0, 0, 0, &map);
    }
    if (ret != kIOReturnSuccess || map == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    X360OutputRequest request;
    bool valid = X360ParseOutputReport(reinterpret_cast<const uint8_t *>(map->GetAddress()),
                                       length < map->GetLength() ? length : map->GetLength(), &request);
    map->release();
    if (!valid) {
        return kIOReturnUnsupported;
    }

    uint8_t command[X360_WIRELESS_COMMAND_SIZE];
    size_t commandLength = 0;
    switch (request.type) {
        case X360_OUTPUT_RUMBLE:
            commandLength = X360BuildRumbleCommand(request.large, request.small, command, sizeof(command));
            break;
        case X360_OUTPUT_LED:
            commandLength = X360BuildLEDCommand(request.pattern, command, sizeof(command));
            break;
        case X360_OUTPUT_POWER_OFF:
            commandLength = X360BuildPowerOffCommand(command, sizeof(command));
            break;
        default:
            return kIOReturnUnsupported;
    }
    ret = ivars->receiver->SendCommand(ivars->slot, command, (uint32_t)commandLength);
    if (ret == kIOReturnSuccess && request.type == X360_OUTPUT_LED) {
        // The controller needs this after an LED change to keep reporting (as in 360Controller).
        commandLength = X360BuildEnableReportingCommand(command, sizeof(command));
        ivars->receiver->SendCommand(ivars->slot, command, (uint32_t)commandLength);
    }
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // We finished synchronously; complete the HID family's request.
    if (action != nullptr) {
        CompleteReport(action, kIOReturnSuccess, (uint32_t)length);
    }
    return kIOReturnSuccess;
}
