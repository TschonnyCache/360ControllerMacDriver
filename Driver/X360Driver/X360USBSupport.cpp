//
//  X360USBSupport.cpp
//  X360Driver
//

#include <DriverKit/OSCollections.h>
#include <USBDriverKit/AppleUSBDescriptorParsing.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "X360USBSupport.h"

bool X360InterfaceMatches(IOUSBHostInterface *interface, int subclass, uint8_t protocol,
                          uint8_t *inAddress, uint8_t *outAddress)
{
    const IOUSBConfigurationDescriptor *configuration = interface->CopyConfigurationDescriptor();
    if (configuration == nullptr) {
        return false;
    }
    const IOUSBInterfaceDescriptor *descriptor = interface->GetInterfaceDescriptor(configuration);
    bool haveIn = false, haveOut = false;
    if (descriptor != nullptr && descriptor->bInterfaceProtocol == protocol
        && (subclass == kX360AnySubclass || descriptor->bInterfaceSubClass == subclass)) {
        const IOUSBEndpointDescriptor *endpoint = nullptr;
        while ((endpoint = IOUSBGetNextEndpointDescriptor(configuration, descriptor,
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
    }
    IOUSBHostFreeDescriptor(configuration);
    return haveIn && haveOut;
}

kern_return_t X360OpenPipes(IOService *owner, IOUSBHostInterface *interface,
                            uint8_t inAddress, uint8_t outAddress,
                            uint32_t inCapacity, uint32_t outCapacity, X360Pipes *pipes)
{
    *pipes = X360Pipes{};
    kern_return_t ret = interface->Open(owner, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        interface->release();
        return ret;
    }
    pipes->interface = interface;
    ret = interface->CopyPipe(inAddress, &pipes->inPipe);
    if (ret == kIOReturnSuccess) {
        ret = interface->CopyPipe(outAddress, &pipes->outPipe);
    }
    if (ret == kIOReturnSuccess) {
        ret = interface->CreateIOBuffer(kIOMemoryDirectionIn, inCapacity, &pipes->inBuffer);
    }
    if (ret == kIOReturnSuccess) {
        ret = interface->CreateIOBuffer(kIOMemoryDirectionOut, outCapacity, &pipes->outBuffer);
    }
    if (ret != kIOReturnSuccess) {
        interface->Close(owner, 0);
        X360ReleasePipes(pipes);
    }
    return ret;
}

void X360AbortAndClosePipes(IOService *owner, X360Pipes *pipes)
{
    if (pipes->inPipe != nullptr) {
        pipes->inPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
    }
    if (pipes->outPipe != nullptr) {
        pipes->outPipe->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, nullptr);
    }
    if (pipes->interface != nullptr) {
        pipes->interface->Close(owner, 0);
    }
}

void X360ReleasePipes(X360Pipes *pipes)
{
    OSSafeReleaseNULL(pipes->inBuffer);
    OSSafeReleaseNULL(pipes->outBuffer);
    OSSafeReleaseNULL(pipes->inPipe);
    OSSafeReleaseNULL(pipes->outPipe);
    OSSafeReleaseNULL(pipes->interface);
}

kern_return_t X360QueueRead(X360Pipes *pipes, uint32_t length, OSAction *completion)
{
    if (pipes->inPipe == nullptr || pipes->inBuffer == nullptr) {
        return kIOReturnNotReady;
    }
    return pipes->inPipe->AsyncIO(pipes->inBuffer, length, completion, 0);
}

kern_return_t X360WritePipe(X360Pipes *pipes, const uint8_t *bytes, uint32_t length)
{
    IOAddressSegment range = {};
    if (pipes->outPipe == nullptr || pipes->outBuffer == nullptr || bytes == nullptr || length == 0
        || pipes->outBuffer->GetAddressRange(&range) != kIOReturnSuccess || range.length < length) {
        return kIOReturnBadArgument;
    }
    uint8_t *destination = reinterpret_cast<uint8_t *>(range.address);
    for (uint32_t i = 0; i < length; i++) {
        destination[i] = bytes[i];
    }
    pipes->outBuffer->SetLength(length);
    uint32_t transferred = 0;
    // Interrupt OUT transfers must use a completion timeout of 0 (verified with x360probe).
    return pipes->outPipe->IO(pipes->outBuffer, length, &transferred, 0);
}

const uint8_t *X360ReadBytes(X360Pipes *pipes, uint32_t actualByteCount, uint32_t *length)
{
    IOAddressSegment range = {};
    if (pipes->inBuffer == nullptr || pipes->inBuffer->GetAddressRange(&range) != kIOReturnSuccess) {
        return nullptr;
    }
    *length = actualByteCount < range.length ? actualByteCount : (uint32_t)range.length;
    return reinterpret_cast<const uint8_t *>(range.address);
}

uint32_t X360CopyLocationID(IOService *provider)
{
    uint32_t locationID = 0;
    OSDictionary *properties = nullptr;
    if (provider->CopyProperties(&properties) == kIOReturnSuccess && properties != nullptr) {
        OSNumber *location = OSDynamicCast(OSNumber, OSDictionaryGetValue(properties, "locationID"));
        if (location != nullptr) {
            locationID = location->unsigned32BitValue();
        }
        properties->release();
    }
    return locationID;
}
