//
//  X360USBSupport.h
//  X360Driver
//
//  USB plumbing shared by the wireless receiver and the wired controller
//  services: finding a controller interface, opening its interrupt pipes and
//  doing synchronous writes.
//

#ifndef X360USBSupport_h
#define X360USBSupport_h

#include <DriverKit/IOService.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>

/// Pass as `subclass` to match an interface of any subclass.
static const int kX360AnySubclass = -1;

/// An opened interface with one interrupt IN and one interrupt OUT pipe and I/O buffers for both.
struct X360Pipes {
    IOUSBHostInterface       *interface;
    IOUSBHostPipe            *inPipe;
    IOUSBHostPipe            *outPipe;
    IOBufferMemoryDescriptor *inBuffer;
    IOBufferMemoryDescriptor *outBuffer;
};

/// Returns true if `interface` has the given subclass/protocol and an interrupt
/// IN and OUT endpoint, whose addresses are returned.
bool X360InterfaceMatches(IOUSBHostInterface *interface, int subclass, uint8_t protocol,
                          uint8_t *inAddress, uint8_t *outAddress);

/// Opens `interface` for `owner`, copies both pipes and allocates buffers.
/// On success `pipes` takes over the caller's reference to `interface`; on failure
/// everything (including that reference) is released and `pipes` is zeroed.
kern_return_t X360OpenPipes(IOService *owner, IOUSBHostInterface *interface,
                            uint8_t inAddress, uint8_t outAddress,
                            uint32_t inCapacity, uint32_t outCapacity, X360Pipes *pipes);

/// Aborts outstanding I/O (asynchronously) and closes the interface. Objects stay
/// allocated until `X360ReleasePipes`, so late completions remain safe.
void X360AbortAndClosePipes(IOService *owner, X360Pipes *pipes);

/// Releases every object held by `pipes` and zeroes it.
void X360ReleasePipes(X360Pipes *pipes);

/// Starts an asynchronous read of the whole IN buffer.
kern_return_t X360QueueRead(X360Pipes *pipes, uint32_t length, OSAction *completion);

/// Writes `bytes` synchronously to the OUT pipe. Callers must serialise access.
kern_return_t X360WritePipe(X360Pipes *pipes, const uint8_t *bytes, uint32_t length);

/// Returns the bytes of a completed read (clamped to the buffer) or nullptr.
const uint8_t *X360ReadBytes(X360Pipes *pipes, uint32_t actualByteCount, uint32_t *length);

/// Reads the provider's "locationID" property (0 if missing).
uint32_t X360CopyLocationID(IOService *provider);

#endif /* X360USBSupport_h */
