//
//  X360WirelessReceiver.cpp
//  X360Driver
//

#include <os/log.h>
#include <time.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSCollections.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/USBDriverKitDefs.h>
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>

#include "X360WirelessReceiver.h"
#include "X360Gamepad.h"
#include "X360USBSupport.h"
#include "X360Protocol.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "X360Driver receiver: " fmt, ##__VA_ARGS__)

/// Give up on a slot after this many consecutive read errors (prevents a busy loop on a dead pipe).
static const uint32_t kMaxConsecutiveReadErrors = 10;

/// Receiver personality keys holding the properties for each slot's gamepad (see Info.plist).
static const char *const kGamepadPropertiesKeys[X360_MAX_SLOTS] = {
    "X360GamepadSlot0", "X360GamepadSlot1", "X360GamepadSlot2", "X360GamepadSlot3",
};

/// Personality property: minutes without input before a pad is switched off (0 = never).
#define kIdlePowerOffMinutesKey   "X360IdlePowerOffMinutes"
static const uint32_t kDefaultIdlePowerOffMinutes = 15;
static const uint64_t kNanosecondsPerMinute = 60ull * 1000 * 1000 * 1000;

struct X360Slot {
    X360Pipes    pipes;
    OSAction    *readAction;
    X360Gamepad *gamepad;
    uint32_t     readErrors;
    uint32_t     idleMinutes;   // Touched only on the default queue (reads + idle timer).
};

struct X360WirelessReceiver_IVars {
    IOUSBHostDevice       *device;
    IOLock                *lock;          // Guards `stopping`, gamepad pointers and OUT pipe use.
    X360Slot               slots[X360_MAX_SLOTS];
    uint32_t               slotCount;
    uint32_t               pendingCancels;
    uint32_t               locationID;
    uint32_t               idlePowerOffMinutes;
    IOTimerDispatchSource *idleTimer;
    OSAction              *idleAction;
    bool                   stopping;
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

void X360WirelessReceiver::free()
{
    if (ivars != nullptr) {
        for (uint32_t i = 0; i < X360_MAX_SLOTS; i++) {
            X360Slot &slot = ivars->slots[i];
            OSSafeReleaseNULL(slot.gamepad);
            OSSafeReleaseNULL(slot.readAction);
            X360ReleasePipes(&slot.pipes);
        }
        OSSafeReleaseNULL(ivars->idleTimer);
        OSSafeReleaseNULL(ivars->idleAction);
        OSSafeReleaseNULL(ivars->device);
        if (ivars->lock != nullptr) {
            IOLockFree(ivars->lock);
        }
    }
    IOSafeDeleteNULL(ivars, X360WirelessReceiver_IVars, 1);
    super::free();
}

/// Reads the idle power-off setting from our personality.
static uint32_t ReadIdlePowerOffMinutes(IOService *service)
{
    uint32_t minutes = kDefaultIdlePowerOffMinutes;
    OSDictionary *properties = nullptr;
    if (service->CopyProperties(&properties) == kIOReturnSuccess && properties != nullptr) {
        OSNumber *value = OSDynamicCast(OSNumber, OSDictionaryGetValue(properties, kIdlePowerOffMinutesKey));
        if (value != nullptr) {
            minutes = value->unsigned32BitValue();
        }
        properties->release();
    }
    return minutes;
}

static void ArmIdleTimer(IOTimerDispatchSource *timer)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    timer->WakeAtTime(kIOTimerClockUptimeRaw, now + kNanosecondsPerMinute, kNanosecondsPerMinute / 10);
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
    ivars->locationID = X360CopyLocationID(provider);
    ivars->idlePowerOffMinutes = ReadIdlePowerOffMinutes(this);

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
    if (ivars->device->CreateInterfaceIterator(&iterator) == kIOReturnSuccess) {
        IOUSBHostInterface *interface = nullptr;
        while (ivars->slotCount < X360_MAX_SLOTS
               && ivars->device->CopyInterface(iterator, &interface) == kIOReturnSuccess && interface != nullptr) {
            uint8_t inAddress = 0, outAddress = 0;
            if (!X360InterfaceMatches(interface, kX360AnySubclass, X360_INTERFACE_PROTOCOL_CONTROLLER,
                                      &inAddress, &outAddress)) {
                interface->release();
                continue;
            }
            X360Slot &slot = ivars->slots[ivars->slotCount];
            if (X360OpenPipes(this, interface, inAddress, outAddress,
                              X360_WIRELESS_MAX_PACKET, X360_WIRELESS_COMMAND_SIZE, &slot.pipes) != kIOReturnSuccess) {
                Log("failed to open slot %u", ivars->slotCount);
                continue;
            }
            if (CreateActionReadComplete(sizeof(uint32_t), &slot.readAction) != kIOReturnSuccess) {
                Log("failed to create read action for slot %u", ivars->slotCount);
                X360AbortAndClosePipes(this, &slot.pipes);
                X360ReleasePipes(&slot.pipes);
                continue;   // The index is reused by the next interface.
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
    Log("started with %u slots, idle power-off after %u min", ivars->slotCount, ivars->idlePowerOffMinutes);

    for (uint32_t i = 0; i < ivars->slotCount; i++) {
        X360Slot &slot = ivars->slots[i];
        if (X360QueueRead(&slot.pipes, X360_WIRELESS_MAX_PACKET, slot.readAction) != kIOReturnSuccess) {
            Log("could not start reading slot %u", i);
        }
        // Ask whether a pad is already linked; the answer arrives as a connection status packet.
        uint8_t inquiry[X360_WIRELESS_COMMAND_SIZE];
        IOLockLock(ivars->lock);
        X360WritePipe(&slot.pipes, inquiry, (uint32_t)X360BuildInquiryCommand(inquiry, sizeof(inquiry)));
        IOLockUnlock(ivars->lock);
    }

    if (ivars->idlePowerOffMinutes > 0) {
        IODispatchQueue *queue = nullptr;
        if (CopyDispatchQueue(kIOServiceDefaultQueueName, &queue) == kIOReturnSuccess
            && IOTimerDispatchSource::Create(queue, &ivars->idleTimer) == kIOReturnSuccess
            && CreateActionIdleTimerFired(0, &ivars->idleAction) == kIOReturnSuccess
            && ivars->idleTimer->SetHandler(ivars->idleAction) == kIOReturnSuccess) {
            ArmIdleTimer(ivars->idleTimer);
        } else {
            Log("idle timer unavailable; controllers won't switch off automatically");
            OSSafeReleaseNULL(ivars->idleTimer);
            OSSafeReleaseNULL(ivars->idleAction);
        }
        OSSafeReleaseNULL(queue);
    }

    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(X360WirelessReceiver, Stop)
{
    Log("stopping");
    X360Gamepad *gamepads[X360_MAX_SLOTS] = {};

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

    // Abort outstanding I/O, then wait until every read action and the timer are
    // cancelled before finishing Stop, so no callback can run against a stopped service.
    ivars->pendingCancels = 0;
    for (uint32_t i = 0; i < ivars->slotCount; i++) {
        X360AbortAndClosePipes(this, &ivars->slots[i].pipes);
        if (ivars->slots[i].readAction != nullptr) {
            ivars->pendingCancels++;
        }
    }
    if (ivars->idleTimer != nullptr) {
        ivars->pendingCancels++;
    }
    if (ivars->device != nullptr) {
        ivars->device->Close(this, 0);
    }

    if (ivars->pendingCancels == 0) {
        return Stop(provider, SUPERDISPATCH);
    }
    // Cancel handlers run on our default queue, so the counter needs no lock.
    auto finishOne = ^{
        if (--ivars->pendingCancels == 0) {
            Stop(provider, SUPERDISPATCH);
        }
    };
    for (uint32_t i = 0; i < ivars->slotCount; i++) {
        if (ivars->slots[i].readAction != nullptr) {
            ivars->slots[i].readAction->Cancel(finishOne);
        }
    }
    if (ivars->idleTimer != nullptr) {
        ivars->idleTimer->Cancel(finishOne);
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
    X360Gamepad *gamepad = service ? OSDynamicCast(X360Gamepad, service) : nullptr;
    if (ret != kIOReturnSuccess || gamepad == nullptr) {
        Log("creating gamepad for slot %u failed 0x%x", index, ret);
        if (service != nullptr) {
            service->Terminate(0);
            service->release();
        }
        return;
    }

    IOLockLock(ivars->lock);
    ivars->slots[index].gamepad = gamepad;   // Keeps the reference from Create.
    ivars->slots[index].idleMinutes = 0;
    uint8_t command[X360_WIRELESS_COMMAND_SIZE];
    X360WritePipe(&ivars->slots[index].pipes, command,
                  (uint32_t)X360BuildLEDCommand(X360LEDPatternForSlot(index), command, sizeof(command)));
    X360WritePipe(&ivars->slots[index].pipes, command,
                  (uint32_t)X360BuildEnableReportingCommand(command, sizeof(command)));
    IOLockUnlock(ivars->lock);
    Log("gamepad published for slot %u", index);
}

static void RemoveGamepad(X360WirelessReceiver_IVars *ivars, uint32_t index)
{
    IOLockLock(ivars->lock);
    X360Gamepad *gamepad = ivars->slots[index].gamepad;
    ivars->slots[index].gamepad = nullptr;
    IOLockUnlock(ivars->lock);
    if (gamepad != nullptr) {
        Log("removing gamepad for slot %u", index);
        gamepad->Terminate(0);
        gamepad->release();
    }
}

/// Returns the slot's gamepad retained (or nullptr). Caller releases.
static X360Gamepad *CopyGamepad(X360WirelessReceiver_IVars *ivars, uint32_t index)
{
    IOLockLock(ivars->lock);
    X360Gamepad *gamepad = ivars->slots[index].gamepad;
    if (gamepad != nullptr) {
        gamepad->retain();
    }
    IOLockUnlock(ivars->lock);
    return gamepad;
}

void X360WirelessReceiver::GamepadDidStop(uint32_t slot, X360Gamepad *gamepad)
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

// MARK: - Output

kern_return_t X360WirelessReceiver::HandleOutputRequest(uint32_t slotIndex, const struct X360OutputRequest *request)
{
    if (slotIndex >= ivars->slotCount || request == nullptr) {
        return kIOReturnBadArgument;
    }
    uint8_t command[X360_WIRELESS_COMMAND_SIZE];
    size_t length = 0;
    switch (request->type) {
        case X360_OUTPUT_RUMBLE:
            length = X360BuildRumbleCommand(request->large, request->small, command, sizeof(command));
            break;
        case X360_OUTPUT_LED:
            length = X360BuildLEDCommand(request->pattern, command, sizeof(command));
            break;
        case X360_OUTPUT_POWER_OFF:
            length = X360BuildPowerOffCommand(command, sizeof(command));
            break;
        default:
            return kIOReturnUnsupported;
    }

    IOLockLock(ivars->lock);
    kern_return_t ret = ivars->stopping ? kIOReturnNotReady
                                        : X360WritePipe(&ivars->slots[slotIndex].pipes, command, (uint32_t)length);
    if (ret == kIOReturnSuccess && request->type == X360_OUTPUT_LED) {
        // The controller needs this after an LED change to keep reporting (as in 360Controller).
        length = X360BuildEnableReportingCommand(command, sizeof(command));
        X360WritePipe(&ivars->slots[slotIndex].pipes, command, (uint32_t)length);
    }
    IOLockUnlock(ivars->lock);

    if (ret != kIOReturnSuccess) {
        Log("command to slot %u failed 0x%x", slotIndex, ret);
    }
    return ret;
}

// MARK: - Input

void IMPL(X360WirelessReceiver, ReadComplete)
{
    uint32_t index = *static_cast<uint32_t *>(action->GetReference());
    if (index >= ivars->slotCount) {
        return;
    }
    X360Slot &slot = ivars->slots[index];

    if (ivars->stopping || status == kIOReturnAborted || status == kIOReturnOffline || status == kIOReturnNoDevice) {
        return;
    }

    if (status == kIOReturnSuccess || status == kIOReturnOverrun) {
        slot.readErrors = 0;
        uint32_t length = 0;
        const uint8_t *bytes = X360ReadBytes(&slot.pipes, actualByteCount, &length);
        X360Event event;
        if (bytes != nullptr && X360ParseWirelessPacket(bytes, length, &event)) {
            switch (event.type) {
                case X360_EVENT_CONNECTED:
                case X360_EVENT_INFO:
                case X360_EVENT_INPUT: {
                    // Pads linked before we started only show up through input or an inquiry answer.
                    EnsureGamepad(this, ivars, index);
                    if (event.type != X360_EVENT_INFO) {
                        slot.idleMinutes = 0;
                    }
                    X360Gamepad *gamepad = CopyGamepad(ivars, index);
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
                    X360Gamepad *gamepad = CopyGamepad(ivars, index);
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
        if (status == kUSBHostReturnPipeStalled && slot.pipes.inPipe != nullptr) {
            slot.pipes.inPipe->ClearStall(true);
        }
        if (++slot.readErrors >= kMaxConsecutiveReadErrors) {
            Log("giving up on slot %u after repeated errors", index);
            RemoveGamepad(ivars, index);
            return;
        }
    }

    if (!ivars->stopping && X360QueueRead(&slot.pipes, X360_WIRELESS_MAX_PACKET, slot.readAction) != kIOReturnSuccess) {
        Log("could not re-queue read on slot %u", index);
    }
}

// MARK: - Idle power-off

void IMPL(X360WirelessReceiver, IdleTimerFired)
{
    if (ivars->stopping) {
        return;
    }
    for (uint32_t i = 0; i < ivars->slotCount; i++) {
        X360Slot &slot = ivars->slots[i];
        IOLockLock(ivars->lock);
        bool active = slot.gamepad != nullptr;
        IOLockUnlock(ivars->lock);
        if (!active) {
            slot.idleMinutes = 0;
            continue;
        }
        if (++slot.idleMinutes >= ivars->idlePowerOffMinutes) {
            // Like 360Controller: save the batteries of forgotten pads. The receiver then
            // reports a disconnect, which removes the gamepad.
            Log("slot %u idle for %u min, switching it off", i, slot.idleMinutes);
            X360OutputRequest powerOff = {};
            powerOff.type = X360_OUTPUT_POWER_OFF;
            HandleOutputRequest(i, &powerOff);
            slot.idleMinutes = 0;
        }
    }
    ArmIdleTimer(ivars->idleTimer);
}
