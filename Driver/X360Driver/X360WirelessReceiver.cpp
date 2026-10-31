//
//  X360WirelessReceiver.cpp
//  X360Driver
//

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSCollections.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/USBDriverKitDefs.h>
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>

#include "X360WirelessReceiver.h"
#include "X360WirelessGamepad.h"
#include "X360Protocol.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "X360Driver receiver: " fmt, ##__VA_ARGS__)

/// Give up on a slot after this many consecutive read errors (prevents a busy loop on a dead pipe).
static const uint32_t kMaxConsecutiveReadErrors = 10;

/// Receiver personality keys holding the properties for each slot's gamepad (see Info.plist).
static const char *const kGamepadPropertiesKeys[X360_MAX_SLOTS] = {
    "X360GamepadSlot0", "X360GamepadSlot1", "X360GamepadSlot2", "X360GamepadSlot3",
};

struct X360Slot {
    IOUSBHostInterface       *interface;
    IOUSBHostPipe            *inPipe;
    IOUSBHostPipe            *outPipe;
    IOBufferMemoryDescriptor *inBuffer;
    IOBufferMemoryDescriptor *outBuffer;
    OSAction                 *readAction;
    X360WirelessGamepad      *gamepad;
    uint32_t                  readErrors;
    bool                      reading;
};

struct X360WirelessReceiver_IVars {
    IOUSBHostDevice *device;
    IOLock          *lock;          // Guards `stopping`, gamepad pointers and OUT pipe use.
    X360Slot         slots[X360_MAX_SLOTS];
    uint32_t         slotCount;
    uint32_t         pendingCancels;
    uint32_t         locationID;
    bool             stopping;
};

// MARK: - Lifecycle

bool X360WirelessReceiver::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(X360WirelessReceiver_IVars, 1);
    if (ivars == nullptr) {
        return false;
    }
    ivars->lock = IOLockAlloc();
    return ivars->lock != nullptr;
}

/// Releases everything a slot holds and zeroes it. Does not close the interface.
static void ReleaseSlot(X360Slot &slot)
{
    OSSafeReleaseNULL(slot.gamepad);
    OSSafeReleaseNULL(slot.readAction);
    OSSafeReleaseNULL(slot.inBuffer);
    OSSafeReleaseNULL(slot.outBuffer);
    OSSafeReleaseNULL(slot.inPipe);
    OSSafeReleaseNULL(slot.outPipe);
    OSSafeReleaseNULL(slot.interface);
    slot = X360Slot{};
}

void X360WirelessReceiver::free()
{
    if (ivars != nullptr) {
        for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
            ReleaseSlot(ivars->slots[i]);
        }
        OSSafeReleaseNULL(ivars->device);
        if (ivars->lock != nullptr) {
            IOLockFree(ivars->lock);
        }
    }
    IOSafeDeleteNULL(ivars, X360WirelessReceiver_IVars, 1);
    super::free();
}

/// Finds the first interrupt IN and OUT endpoints of `interfaceDescriptor`.
static bool FindEndpoints(const IOUSBConfigurationDescriptor *configuration,
                          const IOUSBInterfaceDescriptor *interfaceDescriptor,
                          uint8_t *inAddress, uint8_t *outAddress)
{
    bool haveIn = false, haveOut = false;
    const IOUSBEndpointDescriptor *endpoint = nullptr;
    while ((endpoint = IOUSBGetNextEndpointDescriptor(configuration, interfaceDescriptor,
                                                      reinterpret_cast<const IOUSBDescriptorHeader *>(endpoint))) != nullptr) {
        if (IOUSBGetEndpointType(endpoint) != kIOUSBEndpointTypeInterrupt) {
            continue;
        }
        if (IOUSBGetEndpointDirection(endpoint) == kIOUSBEndpointDirectionIn) {
            if (!haveIn) { *inAddress = IOUSBGetEndpointAddress(endpoint); haveIn = true; }
        } else {
            if (!haveOut) { *outAddress = IOUSBGetEndpointAddress(endpoint); haveOut = true; }
        }
    }
    return haveIn && haveOut;
}

kern_return_t IMPL(X360WirelessReceiver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super Start failed 0x%x", ret);
        return ret;
    }

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Log("provider is not an IOUSBHostDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->device->retain();

    OSDictionary *providerProperties = nullptr;
    if (provider->CopyProperties(&providerProperties) == kIOReturnSuccess && providerProperties != nullptr) {
        OSNumber *location = OSDynamicCast(OSNumber, OSDictionaryGetValue(providerProperties, "locationID"));
        if (location != nullptr) {
            ivars->locationID = location->unsigned32BitValue();
        }
        providerProperties->release();
    }

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed 0x%x (is another client holding it exclusively?)", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    // Select the (only) configuration without letting other drivers match its interfaces.
    const IOUSBConfigurationDescriptor *configuration = ivars->device->CopyConfigurationDescriptor((uint8_t)0);
    if (configuration == nullptr) {
        Log("no configuration descriptor");
        ivars->device->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoResources;
    }
    ret = ivars->device->SetConfiguration(configuration->bConfigurationValue, false);
    IOUSBHostFreeDescriptor(configuration);
    if (ret != kIOReturnSuccess) {
        Log("SetConfiguration failed 0x%x", ret);
        ivars->device->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    // Collect the controller interface (protocol 0x81) of every slot.
    uintptr_t iterator = 0;
    ret = ivars->device->CreateInterfaceIterator(&iterator);
    if (ret == kIOReturnSuccess) {
        IOUSBHostInterface *interface = nullptr;
        while (ivars->slotCount < X360_MAX_SLOTS
               && ivars->device->CopyInterface(iterator, &interface) == kIOReturnSuccess && interface != nullptr) {
            const IOUSBConfigurationDescriptor *interfaceConfiguration = interface->CopyConfigurationDescriptor();
            const IOUSBInterfaceDescriptor *descriptor =
                interfaceConfiguration ? interface->GetInterfaceDescriptor(interfaceConfiguration) : nullptr;
            uint8_t inAddress = 0, outAddress = 0;
            bool usable = descriptor != nullptr
                && descriptor->bInterfaceProtocol == X360_INTERFACE_PROTOCOL_CONTROLLER
                && FindEndpoints(interfaceConfiguration, descriptor, &inAddress, &outAddress);
            if (interfaceConfiguration != nullptr) {
                IOUSBHostFreeDescriptor(interfaceConfiguration);
            }
            if (!usable || interface->Open(this, 0, nullptr) != kIOReturnSuccess) {
                interface->release();
                interface = nullptr;
                continue;
            }

            X360Slot &slot = ivars->slots[ivars->slotCount];
            slot.interface = interface;     // Takes the reference from CopyInterface.
            interface = nullptr;
            if (slot.interface->CopyPipe(inAddress, &slot.inPipe) != kIOReturnSuccess
                || slot.interface->CopyPipe(outAddress, &slot.outPipe) != kIOReturnSuccess
                || slot.interface->CreateIOBuffer(kIOMemoryDirectionIn, X360_WIRELESS_MAX_PACKET, &slot.inBuffer) != kIOReturnSuccess
                || slot.interface->CreateIOBuffer(kIOMemoryDirectionOut, X360_WIRELESS_COMMAND_SIZE, &slot.outBuffer) != kIOReturnSuccess
                || CreateActionReadComplete(sizeof(uint32_t), &slot.readAction) != kIOReturnSuccess) {
                Log("failed to set up slot %u", ivars->slotCount);
                slot.interface->Close(this, 0);
                ReleaseSlot(slot);      // The index is reused by the next interface.
                continue;
            }
            *static_cast<uint32_t *>(slot.readAction->GetReference()) = ivars->slotCount;
            ivars->slotCount++;
        }
        ivars->device->DestroyInterfaceIterator(iterator);
    }

    if (ivars->slotCount == 0) {
        Log("no controller slots found");
        ivars->device->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNotFound;
    }
    Log("started with %u slots", ivars->slotCount);

    for (uint32_t i = 0; i < ivars->slotCount; i++) {
        X360Slot &slot = ivars->slots[i];
        slot.reading = slot.inPipe->AsyncIO(slot.inBuffer, X360_WIRELESS_MAX_PACKET, slot.readAction, 0) == kIOReturnSuccess;
        // Ask whether a pad is already linked; the answer arrives as a connection status packet.
        uint8_t inquiry[X360_WIRELESS_COMMAND_SIZE];
        SendCommand(i, inquiry, (uint32_t)X360BuildInquiryCommand(inquiry, sizeof(inquiry)));
    }

    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(X360WirelessReceiver, Stop)
{
    Log("stopping");
    X360WirelessGamepad *gamepads[X360_MAX_SLOTS] = {};

    IOLockLock(ivars->lock);
    ivars->stopping = true;
    for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
        gamepads[i] = ivars->slots[i].gamepad;
        ivars->slots[i].gamepad = nullptr;
    }
    IOLockUnlock(ivars->lock);

    for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
        if (gamepads[i] != nullptr) {
            gamepads[i]->Terminate(0);
            gamepads[i]->release();
        }
    }

    // Abort outstanding reads, then wait for every read action to be cancelled
    // before finishing Stop, so no completion can run against a stopped service.
    ivars->pendingCancels = 0;
    for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
        X360Slot &slot = ivars->slots[i];
        if (slot.inPipe != nullptr) {
            slot.inPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
        }
        if (slot.outPipe != nullptr) {
            slot.outPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
        }
        if (slot.interface != nullptr) {
            slot.interface->Close(this, 0);
        }
        if (slot.readAction != nullptr) {
            ivars->pendingCancels++;
        }
    }
    if (ivars->device != nullptr) {
        ivars->device->Close(this, 0);
    }

    if (ivars->pendingCancels == 0) {
        return Stop(provider, SUPERDISPATCH);
    }
    for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
        OSAction *action = ivars->slots[i].readAction;
        if (action == nullptr) {
            continue;
        }
        // Cancel handlers run on our default queue, so the counter needs no lock.
        action->Cancel(^{
            if (--ivars->pendingCancels == 0) {
                Stop(provider, SUPERDISPATCH);
            }
        });
    }
    return kIOReturnSuccess;
}

// MARK: - Gamepads

/// Publishes a gamepad for `index` if there isn't one yet.
static void EnsureGamepad(X360WirelessReceiver *self, X360WirelessReceiver_IVars *ivars, uint32_t index)
{
    IOLockLock(ivars->lock);
    bool exists = ivars->slots[index].gamepad != nullptr || ivars->stopping;
    IOLockUnlock(ivars->lock);
    if (exists) {
        return;
    }

    IOService *service = nullptr;
    kern_return_t ret = self->Create(self, kGamepadPropertiesKeys[index], &service);
    X360WirelessGamepad *gamepad = service ? OSDynamicCast(X360WirelessGamepad, service) : nullptr;
    if (ret != kIOReturnSuccess || gamepad == nullptr) {
        os_log(OS_LOG_DEFAULT, "X360Driver receiver: creating gamepad for slot %u failed 0x%x", index, ret);
        if (service != nullptr) {
            service->Terminate(0);
            service->release();
        }
        return;
    }

    IOLockLock(ivars->lock);
    ivars->slots[index].gamepad = gamepad;   // Keeps the reference from Create.
    IOLockUnlock(ivars->lock);
    os_log(OS_LOG_DEFAULT, "X360Driver receiver: gamepad published for slot %u", index);

    uint8_t command[X360_WIRELESS_COMMAND_SIZE];
    self->SendCommand(index, command, (uint32_t)X360BuildLEDCommand(X360LEDPatternForSlot(index), command, sizeof(command)));
    self->SendCommand(index, command, (uint32_t)X360BuildEnableReportingCommand(command, sizeof(command)));
}

static void RemoveGamepad(X360WirelessReceiver_IVars *ivars, uint32_t index)
{
    IOLockLock(ivars->lock);
    X360WirelessGamepad *gamepad = ivars->slots[index].gamepad;
    ivars->slots[index].gamepad = nullptr;
    IOLockUnlock(ivars->lock);
    if (gamepad != nullptr) {
        os_log(OS_LOG_DEFAULT, "X360Driver receiver: removing gamepad for slot %u", index);
        gamepad->Terminate(0);
        gamepad->release();
    }
}

/// Returns the slot's gamepad retained (or nullptr). Caller releases.
static X360WirelessGamepad *CopyGamepad(X360WirelessReceiver_IVars *ivars, uint32_t index)
{
    IOLockLock(ivars->lock);
    X360WirelessGamepad *gamepad = ivars->slots[index].gamepad;
    if (gamepad != nullptr) {
        gamepad->retain();
    }
    IOLockUnlock(ivars->lock);
    return gamepad;
}

void X360WirelessReceiver::GamepadDidStop(uint32_t slot, X360WirelessGamepad *gamepad)
{
    if (slot >= X360_MAX_SLOTS) {
        return;
    }
    IOLockLock(ivars->lock);
    if (ivars->slots[slot].gamepad == gamepad) {
        // Stopped by the system rather than by us: drop our reference.
        ivars->slots[slot].gamepad = nullptr;
        IOLockUnlock(ivars->lock);
        gamepad->release();
        return;
    }
    IOLockUnlock(ivars->lock);
}

uint32_t X360WirelessReceiver::GetLocationID()
{
    return ivars->locationID;
}

// MARK: - I/O

kern_return_t X360WirelessReceiver::SendCommand(uint32_t slotIndex, const uint8_t *bytes, uint32_t length)
{
    if (slotIndex >= ivars->slotCount || bytes == nullptr || length == 0 || length > X360_WIRELESS_COMMAND_SIZE) {
        return kIOReturnBadArgument;
    }
    X360Slot &slot = ivars->slots[slotIndex];
    IOAddressSegment range = {};

    IOLockLock(ivars->lock);
    kern_return_t ret = kIOReturnNotReady;
    if (!ivars->stopping && slot.outPipe != nullptr && slot.outBuffer != nullptr
        && slot.outBuffer->GetAddressRange(&range) == kIOReturnSuccess && range.length >= length) {
        uint8_t *destination = reinterpret_cast<uint8_t *>(range.address);
        for (uint32_t i = 0; i < length; i++) {
            destination[i] = bytes[i];
        }
        slot.outBuffer->SetLength(length);
        uint32_t transferred = 0;
        // Interrupt OUT transfers must use a completion timeout of 0 (verified with x360probe).
        ret = slot.outPipe->IO(slot.outBuffer, length, &transferred, 0);
    }
    IOLockUnlock(ivars->lock);

    if (ret != kIOReturnSuccess) {
        Log("command to slot %u failed 0x%x", slotIndex, ret);
    }
    return ret;
}

void IMPL(X360WirelessReceiver, ReadComplete)
{
    uint32_t index = *static_cast<uint32_t *>(action->GetReference());
    if (index >= ivars->slotCount) {
        return;
    }
    X360Slot &slot = ivars->slots[index];
    slot.reading = false;

    if (ivars->stopping || status == kIOReturnAborted || status == kIOReturnOffline || status == kIOReturnNoDevice) {
        return;
    }

    if (status == kIOReturnSuccess || status == kIOReturnOverrun) {
        slot.readErrors = 0;
        IOAddressSegment range = {};
        X360Event event;
        if (slot.inBuffer->GetAddressRange(&range) == kIOReturnSuccess
            && X360ParseWirelessPacket(reinterpret_cast<const uint8_t *>(range.address),
                                       actualByteCount < range.length ? actualByteCount : range.length, &event)) {
            switch (event.type) {
                case X360_EVENT_CONNECTED:
                case X360_EVENT_INFO:
                case X360_EVENT_INPUT: {
                    // Pads linked before we started only show up through input or an inquiry answer.
                    EnsureGamepad(this, ivars, index);
                    X360WirelessGamepad *gamepad = CopyGamepad(ivars, index);
                    if (gamepad != nullptr) {
                        if (event.type == X360_EVENT_INPUT) {
                            gamepad->PostPadState(&event.state, completionTimestamp);
                        } else if (event.hasBattery) {
                            gamepad->SetBatteryLevel(event.battery);
                        }
                        gamepad->release();
                    }
                    break;
                }
                case X360_EVENT_BATTERY: {
                    X360WirelessGamepad *gamepad = CopyGamepad(ivars, index);
                    if (gamepad != nullptr) {
                        gamepad->SetBatteryLevel(event.battery);
                        gamepad->release();
                    }
                    break;
                }
                case X360_EVENT_DISCONNECTED:
                    RemoveGamepad(ivars, index);
                    break;
                default:
                    break;
            }
        }
    } else {
        Log("read on slot %u failed 0x%x", index, status);
        if (status == kUSBHostReturnPipeStalled) {
            slot.inPipe->ClearStall(true);
        }
        if (++slot.readErrors >= kMaxConsecutiveReadErrors) {
            Log("giving up on slot %u after repeated errors", index);
            RemoveGamepad(ivars, index);
            return;
        }
    }

    if (!ivars->stopping) {
        slot.reading = slot.inPipe->AsyncIO(slot.inBuffer, X360_WIRELESS_MAX_PACKET, slot.readAction, 0) == kIOReturnSuccess;
        if (!slot.reading) {
            Log("could not re-queue read on slot %u", index);
        }
    }
}
