//
//  X360WiredController.cpp
//  X360Driver
//

#include <os/log.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/OSCollections.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/USBDriverKitDefs.h>
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>

#include "X360WiredController.h"
#include "X360Gamepad.h"
#include "X360USBSupport.h"
#include "X360Protocol.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "X360Driver wired: " fmt, ##__VA_ARGS__)

static const uint32_t kMaxConsecutiveReadErrors = 10;

/// Personality key holding the gamepad's properties (see Info.plist).
#define kGamepadPropertiesKey "X360GamepadWired"

struct X360WiredController_IVars {
    IOUSBHostDevice *device;
    IOLock          *lock;          // Guards `stopping`, `gamepad` and OUT pipe use.
    X360Pipes        pipes;
    OSAction        *readAction;
    X360Gamepad     *gamepad;
    uint32_t         locationID;
    uint32_t         readErrors;
    bool             stopping;
};

// MARK: - Lifecycle

bool X360WiredController::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(X360WiredController_IVars, 1);
    if (ivars == nullptr) {
        return false;
    }
    ivars->lock = IOLockAlloc();
    return ivars->lock != nullptr;
}

void X360WiredController::free()
{
    if (ivars != nullptr) {
        OSSafeReleaseNULL(ivars->gamepad);
        OSSafeReleaseNULL(ivars->readAction);
        X360ReleasePipes(&ivars->pipes);
        OSSafeReleaseNULL(ivars->device);
        if (ivars->lock != nullptr) {
            IOLockFree(ivars->lock);
        }
    }
    IOSafeDeleteNULL(ivars, X360WiredController_IVars, 1);
    super::free();
}

/// Opens the controller interface (subclass 0x5D, protocol 0x01) of the configured device.
static kern_return_t OpenControllerInterface(IOService *owner, IOUSBHostDevice *device, X360Pipes *pipes)
{
    uintptr_t iterator = 0;
    kern_return_t ret = device->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = kIOReturnNotFound;
    IOUSBHostInterface *interface = nullptr;
    while (device->CopyInterface(iterator, &interface) == kIOReturnSuccess && interface != nullptr) {
        uint8_t inAddress = 0, outAddress = 0;
        if (X360InterfaceMatches(interface, X360_WIRED_INTERFACE_SUBCLASS, X360_WIRED_INTERFACE_PROTOCOL,
                                 &inAddress, &outAddress)) {
            ret = X360OpenPipes(owner, interface, inAddress, outAddress,
                                X360_WIRED_MAX_PACKET, X360_WIRED_COMMAND_MAX, pipes);
            break;
        }
        interface->release();
    }
    device->DestroyInterfaceIterator(iterator);
    return ret;
}

kern_return_t IMPL(X360WiredController, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->device->retain();
    ivars->locationID = X360CopyLocationID(provider);

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    const IOUSBConfigurationDescriptor *configuration = ivars->device->CopyConfigurationDescriptor((uint8_t)0);
    ret = configuration ? ivars->device->SetConfiguration(configuration->bConfigurationValue, false) : kIOReturnNoResources;
    if (configuration != nullptr) {
        IOUSBHostFreeDescriptor(configuration);
    }
    if (ret == kIOReturnSuccess) {
        ret = OpenControllerInterface(this, ivars->device, &ivars->pipes);
    }
    if (ret == kIOReturnSuccess) {
        ret = CreateActionReadComplete(0, &ivars->readAction);
    }
    if (ret != kIOReturnSuccess) {
        Log("setup failed 0x%x", ret);
        X360AbortAndClosePipes(this, &ivars->pipes);
        X360ReleasePipes(&ivars->pipes);
        ivars->device->Close(this, 0);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    if (X360QueueRead(&ivars->pipes, X360_WIRED_MAX_PACKET, ivars->readAction) != kIOReturnSuccess) {
        Log("could not start reading");
    }

    IOService *service = nullptr;
    ret = Create(this, kGamepadPropertiesKey, &service);
    ivars->gamepad = service ? OSDynamicCast(X360Gamepad, service) : nullptr;
    if (ret != kIOReturnSuccess || ivars->gamepad == nullptr) {
        Log("creating gamepad failed 0x%x", ret);
        if (service != nullptr) {
            service->Terminate(0);
            service->release();
        }
        ivars->gamepad = nullptr;
    }

    // Stop the "searching" animation; without a global player index we show player 1.
    uint8_t command[X360_WIRED_COMMAND_MAX];
    IOLockLock(ivars->lock);
    X360WritePipe(&ivars->pipes, command,
                  (uint32_t)X360BuildWiredLEDCommand(X360_LED_FLASH_THEN_ON_1, command, sizeof(command)));
    IOLockUnlock(ivars->lock);

    Log("started");
    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t IMPL(X360WiredController, Stop)
{
    Log("stopping");
    IOLockLock(ivars->lock);
    ivars->stopping = true;
    X360Gamepad *gamepad = ivars->gamepad;
    ivars->gamepad = nullptr;
    IOLockUnlock(ivars->lock);
    if (gamepad != nullptr) {
        gamepad->Terminate(0);
        gamepad->release();
    }

    X360AbortAndClosePipes(this, &ivars->pipes);
    if (ivars->device != nullptr) {
        ivars->device->Close(this, 0);
    }
    if (ivars->readAction == nullptr) {
        return Stop(provider, SUPERDISPATCH);
    }
    // Finish only once no read completion can run any more.
    ivars->readAction->Cancel(^{
        Stop(provider, SUPERDISPATCH);
    });
    return kIOReturnSuccess;
}

void X360WiredController::GamepadDidStop(X360Gamepad *gamepad)
{
    IOLockLock(ivars->lock);
    bool ours = ivars->gamepad == gamepad;
    if (ours) {
        ivars->gamepad = nullptr;
    }
    IOLockUnlock(ivars->lock);
    if (ours) {
        gamepad->release();
    }
}

uint32_t X360WiredController::GetLocationID()
{
    return ivars->locationID;
}

// MARK: - I/O

kern_return_t X360WiredController::HandleOutputRequest(const struct X360OutputRequest *request)
{
    if (request == nullptr) {
        return kIOReturnBadArgument;
    }
    uint8_t command[X360_WIRED_COMMAND_MAX];
    size_t length = 0;
    switch (request->type) {
        case X360_OUTPUT_RUMBLE:
            length = X360BuildWiredRumbleCommand(request->large, request->small, command, sizeof(command));
            break;
        case X360_OUTPUT_LED:
            length = X360BuildWiredLEDCommand(request->pattern, command, sizeof(command));
            break;
        default:
            return kIOReturnUnsupported;    // Wired pads can't be switched off.
    }
    IOLockLock(ivars->lock);
    kern_return_t ret = ivars->stopping ? kIOReturnNotReady : X360WritePipe(&ivars->pipes, command, (uint32_t)length);
    IOLockUnlock(ivars->lock);
    if (ret != kIOReturnSuccess) {
        Log("command failed 0x%x", ret);
    }
    return ret;
}

void IMPL(X360WiredController, ReadComplete)
{
    if (ivars->stopping || status == kIOReturnAborted || status == kIOReturnOffline || status == kIOReturnNoDevice) {
        return;
    }
    if (status == kIOReturnSuccess || status == kIOReturnOverrun) {
        ivars->readErrors = 0;
        uint32_t length = 0;
        const uint8_t *bytes = X360ReadBytes(&ivars->pipes, actualByteCount, &length);
        X360Event event;
        if (bytes != nullptr && X360ParseWiredPacket(bytes, length, &event)) {
            IOLockLock(ivars->lock);
            X360Gamepad *gamepad = ivars->gamepad;
            if (gamepad != nullptr) {
                gamepad->retain();
            }
            IOLockUnlock(ivars->lock);
            if (gamepad != nullptr) {
                gamepad->PostPadState(&event.state, completionTimestamp);
                gamepad->release();
            }
        }
    } else {
        Log("read failed 0x%x", status);
        if (status == kUSBHostReturnPipeStalled && ivars->pipes.inPipe != nullptr) {
            ivars->pipes.inPipe->ClearStall(true);
        }
        if (++ivars->readErrors >= kMaxConsecutiveReadErrors) {
            Log("giving up after repeated errors");
            return;
        }
    }
    if (!ivars->stopping && X360QueueRead(&ivars->pipes, X360_WIRED_MAX_PACKET, ivars->readAction) != kIOReturnSuccess) {
        Log("could not re-queue read");
    }
}
