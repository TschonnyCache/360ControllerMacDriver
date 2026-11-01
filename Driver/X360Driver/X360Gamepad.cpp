//
//  X360Gamepad.cpp
//  X360Driver
//

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSCollections.h>
#include <HIDDriverKit/IOHIDDeviceKeys.h>

#include "X360Gamepad.h"
#include "X360WirelessReceiver.h"
#include "X360WiredController.h"
#include "X360Protocol.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "X360Driver gamepad: " fmt, ##__VA_ARGS__)

/// Property (set per gamepad in Info.plist) telling the gamepad which receiver slot it represents.
#define kX360SlotKey          "X360Slot"
/// Same property name 360Controller used, so existing tools can read it.
#define kX360BatteryLevelKey  "BatteryLevel"

struct X360Gamepad_IVars {
    // Exactly one of these is set: our provider, which outlives us (not retained).
    X360WirelessReceiver     *receiver;
    X360WiredController      *wired;
    IOBufferMemoryDescriptor *report;
    uint32_t                  slot;
};

// MARK: - Lifecycle

bool X360Gamepad::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(X360Gamepad_IVars, 1);
    return ivars != nullptr;
}

void X360Gamepad::free()
{
    if (ivars != nullptr) {
        OSSafeReleaseNULL(ivars->report);
    }
    IOSafeDeleteNULL(ivars, X360Gamepad_IVars, 1);
    super::free();
}

kern_return_t IMPL(X360Gamepad, Start)
{
    // Resolve provider and slot first: super's Start asks for the device description.
    ivars->receiver = OSDynamicCast(X360WirelessReceiver, provider);
    ivars->wired = OSDynamicCast(X360WiredController, provider);
    if (ivars->receiver == nullptr && ivars->wired == nullptr) {
        Log("unexpected provider");
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
    Log("started (%s, slot %u)", ivars->wired ? "wired" : "wireless", ivars->slot);
    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(X360Gamepad, Stop)
{
    Log("stopping slot %u", ivars->slot);
    if (ivars->receiver != nullptr) {
        ivars->receiver->GamepadDidStop(ivars->slot, this);
    }
    if (ivars->wired != nullptr) {
        ivars->wired->GamepadDidStop(this);
    }
    ivars->receiver = nullptr;
    ivars->wired = nullptr;
    return Stop(provider, SUPERDISPATCH);
}

bool X360Gamepad::handleStart(IOService *provider)
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

OSDictionary *X360Gamepad::newDeviceDescription()
{
    OSDictionary *description = OSDictionary::withCapacity(12);
    if (description == nullptr) {
        return nullptr;
    }
    bool wired = ivars->wired != nullptr;
    uint32_t location = wired ? ivars->wired->GetLocationID()
                              : (ivars->receiver ? ivars->receiver->GetLocationID() : 0);
    char serial[] = "X360-SLOT-0";
    serial[sizeof(serial) - 2] = (char)('1' + ivars->slot);

    // Identity of the pads the 360Controller kext published, so SDL/game mappings apply.
    SetNumber(description, kIOHIDVendorIDKey, X360_VENDOR_MICROSOFT);
    SetNumber(description, kIOHIDProductIDKey, X360_PRODUCT_WIRED_CONTROLLER);
    SetNumber(description, kIOHIDVersionNumberKey, 0x0114);
    SetString(description, kIOHIDTransportKey, wired ? "USB" : "Wireless");
    SetString(description, kIOHIDManufacturerKey, "Microsoft");
    SetString(description, kIOHIDProductKey, wired ? "Xbox 360 Wired Controller" : "Xbox 360 Wireless Controller");
    SetString(description, kIOHIDSerialNumberKey, wired ? "X360-WIRED" : serial);
    // Distinct location per pad: some games use it to tell pads apart.
    SetNumber(description, kIOHIDLocationIDKey, wired ? location : location + ivars->slot + 1);
    SetNumber(description, kIOHIDPrimaryUsagePageKey, 0x01);   // Generic Desktop
    SetNumber(description, kIOHIDPrimaryUsageKey, 0x05);       // Game Pad
    SetNumber(description, kIOHIDCountryCodeKey, 0);
    SetNumber(description, kIOHIDReportIntervalKey, wired ? 4000 : 8000);   // µs
    return description;
}

OSData *X360Gamepad::newReportDescriptor()
{
    return OSData::withBytes(X360HIDReportDescriptor, X360HIDReportDescriptorSize);
}

// MARK: - Input

void X360Gamepad::PostPadState(const struct X360PadState *state, uint64_t timestamp)
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

void X360Gamepad::SetBatteryLevel(uint8_t level)
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

kern_return_t X360Gamepad::setReport(IOMemoryDescriptor *report,
                                     IOHIDReportType reportType,
                                     IOOptionBits options,
                                     uint32_t completionTimeout,
                                     OSAction *action)
{
    if (report == nullptr) {
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

    if (ivars->receiver != nullptr) {
        ret = ivars->receiver->HandleOutputRequest(ivars->slot, &request);
    } else if (ivars->wired != nullptr) {
        ret = ivars->wired->HandleOutputRequest(&request);
    } else {
        ret = kIOReturnNotAttached;
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
