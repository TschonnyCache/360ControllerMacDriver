//
//  x360probe — talks to an Xbox 360 Wireless Receiver from user space (no
//  driver, no signing) to validate the protocol implementation in Core/.
//
//  Usage: x360probe [--seconds N] [--rumble] [--raw]
//

import Foundation
import IOKit
import IOUSBHost
import X360Protocol

// MARK: - Options

struct Options {
    var seconds: Double = 20
    var rumble = false
    var raw = false

    init(arguments: [String]) {
        var iterator = arguments.dropFirst().makeIterator()
        while let argument = iterator.next() {
            switch argument {
            case "--seconds": seconds = iterator.next().flatMap(Double.init) ?? seconds
            case "--rumble": rumble = true
            case "--raw": raw = true
            default:
                print("usage: x360probe [--seconds N] [--rumble] [--raw]")
                exit(argument == "--help" ? 0 : 64)
            }
        }
    }
}

let receiverProductIDs: Set<Int> = [
    Int(X360_PRODUCT_WIRELESS_RECEIVER),
    Int(X360_PRODUCT_WIRELESS_RECEIVER_2),
    Int(X360_PRODUCT_WIRELESS_RECEIVER_3),
]

// MARK: - Registry helpers

func intProperty(_ service: io_service_t, _ key: String) -> Int? {
    IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue() as? Int
}

func findReceiver() -> io_service_t? {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator) == KERN_SUCCESS else {
        return nil
    }
    defer { IOObjectRelease(iterator) }
    while case let service = IOIteratorNext(iterator), service != 0 {
        if intProperty(service, "idVendor") == Int(X360_VENDOR_MICROSOFT),
           let product = intProperty(service, "idProduct"), receiverProductIDs.contains(product) {
            return service
        }
        IOObjectRelease(service)
    }
    return nil
}

/// Controller interfaces (protocol 0x81) that are children of `device`, sorted by interface number.
func controllerInterfaces(of device: io_service_t) -> [io_service_t] {
    var iterator: io_iterator_t = 0
    guard IORegistryEntryGetChildIterator(device, kIOServicePlane, &iterator) == KERN_SUCCESS else { return [] }
    defer { IOObjectRelease(iterator) }
    var found: [(Int, io_service_t)] = []
    while case let child = IOIteratorNext(iterator), child != 0 {
        if IOObjectConformsTo(child, "IOUSBHostInterface") != 0,
           intProperty(child, "bInterfaceProtocol") == Int(X360_INTERFACE_PROTOCOL_CONTROLLER) {
            found.append((intProperty(child, "bInterfaceNumber") ?? 0, child))
        } else {
            IOObjectRelease(child)
        }
    }
    return found.sorted { $0.0 < $1.0 }.map(\.1)
}

func hex(_ bytes: some Sequence<UInt8>) -> String {
    bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
}

func describe(_ state: X360PadState) -> String {
    let names: [(Int, String)] = [
        (X360_BUTTON_A, "A"), (X360_BUTTON_B, "B"), (X360_BUTTON_X, "X"), (X360_BUTTON_Y, "Y"),
        (X360_BUTTON_LB, "LB"), (X360_BUTTON_RB, "RB"), (X360_BUTTON_START, "Start"),
        (X360_BUTTON_BACK, "Back"), (X360_BUTTON_GUIDE, "Guide"), (X360_BUTTON_LSTICK, "LS"),
        (X360_BUTTON_RSTICK, "RS"), (X360_BUTTON_DPAD_UP, "Up"), (X360_BUTTON_DPAD_DOWN, "Down"),
        (X360_BUTTON_DPAD_LEFT, "Left"), (X360_BUTTON_DPAD_RIGHT, "Right"),
    ]
    let pressed = names.filter { Int(state.buttons) & $0.0 != 0 }.map(\.1)
    return String(format: "L(%6d,%6d) R(%6d,%6d) LT %3d RT %3d ",
                  state.leftX, state.leftY, state.rightX, state.rightY,
                  state.leftTrigger, state.rightTrigger)
        + (pressed.isEmpty ? "-" : pressed.joined(separator: " "))
}

// MARK: - Slot

/// One controller slot of the receiver. All mutable state is confined to the shared serial queue.
final class Slot: @unchecked Sendable {
    let index: Int
    let interface: IOUSBHostInterface
    let inPipe: IOUSBHostPipe
    let outPipe: IOUSBHostPipe
    let options: Options
    var lastState: X360PadState?
    var connected = false

    init(index: Int, interface: IOUSBHostInterface, inPipe: IOUSBHostPipe, outPipe: IOUSBHostPipe, options: Options) {
        self.index = index
        self.interface = interface
        self.inPipe = inPipe
        self.outPipe = outPipe
        self.options = options
    }

    func log(_ message: String) {
        print("[slot \(index)] \(message)")
    }

    func send(_ build: (UnsafeMutablePointer<UInt8>, Int) -> Int, _ what: String) {
        let data = NSMutableData(length: Int(X360_WIRELESS_COMMAND_SIZE))!
        data.length = build(data.mutableBytes.assumingMemoryBound(to: UInt8.self), data.length)
        do {
            try outPipe.enqueueIORequest(with: data, completionTimeout: 0) { [self] status, _ in
                if status != kIOReturnSuccess { log("write \(what) failed: \(String(format: "0x%08X", status))") }
            }
        } catch {
            log("write \(what) failed: \(error)")
        }
    }

    func start() {
        send({ X360BuildInquiryCommand($0, $1) }, "inquiry")
        queueRead()
    }

    func queueRead() {
        let data = NSMutableData(length: Int(X360_WIRELESS_MAX_PACKET))!
        do {
            try inPipe.enqueueIORequest(with: data, completionTimeout: 0) { [self] status, count in
                if status == kIOReturnSuccess {
                    handle(Array(UnsafeRawBufferPointer(start: data.bytes, count: count)))
                    queueRead()
                } else if status != kIOReturnAborted {
                    log("read failed: \(String(format: "0x%08X", status)) — stopping this slot")
                }
            }
        } catch {
            log("enqueue read failed: \(error)")
        }
    }

    func handle(_ packet: [UInt8]) {
        if options.raw { log("raw \(hex(packet))") }
        var event = X360Event()
        guard packet.withUnsafeBufferPointer({ X360ParseWirelessPacket($0.baseAddress, $0.count, &event) }) else {
            return
        }
        switch event.type {
        case X360_EVENT_CONNECTED:
            log("controller connected\(event.headset ? " (+ headset)" : "")")
            becomeActive()
        case X360_EVENT_DISCONNECTED:
            log("disconnected")
            connected = false
            lastState = nil
        case X360_EVENT_INFO:
            log("announcement, serial \(hex([event.serial.0, event.serial.1, event.serial.2, event.serial.3]))"
                + (event.hasBattery ? ", battery \(event.battery)" : ""))
            becomeActive()
        case X360_EVENT_BATTERY:
            log("battery \(event.battery)")
        case X360_EVENT_INPUT:
            // Pads that were linked before we started never send a connect packet; input proves presence.
            if !connected { becomeActive() }
            let state = event.state
            if let last = lastState, last.buttons == state.buttons, last.leftTrigger == state.leftTrigger,
               last.rightTrigger == state.rightTrigger, last.leftX == state.leftX, last.leftY == state.leftY,
               last.rightX == state.rightX, last.rightY == state.rightY {
                return
            }
            lastState = state
            log(describe(state))
        default:
            break
        }
    }

    func becomeActive() {
        guard !connected else { return }
        connected = true
        let pattern = UInt8(X360LEDPatternForSlot(UInt32(index)).rawValue)
        send({ X360BuildLEDCommand(pattern, $0, $1) }, "LED")
        send({ X360BuildEnableReportingCommand($0, $1) }, "enable")
        if options.rumble {
            send({ X360BuildRumbleCommand(0x80, 0x80, $0, $1) }, "rumble")
            interface.queue.asyncAfter(deadline: .now() + 0.4) { [self] in
                send({ X360BuildRumbleCommand(0, 0, $0, $1) }, "rumble off")
            }
        }
    }
}

/// Finds the first interrupt IN and OUT endpoint addresses of an interface.
func endpointAddresses(of interface: IOUSBHostInterface) -> (in: UInt8, out: UInt8)? {
    let configuration = interface.configurationDescriptor
    let interfaceDescriptor = interface.interfaceDescriptor
    var inAddress: UInt8?
    var outAddress: UInt8?
    var current = IOUSBGetNextEndpointDescriptor(configuration, interfaceDescriptor, nil)
    while let endpoint = current {
        let address = endpoint.pointee.bEndpointAddress
        if endpoint.pointee.bmAttributes & 0x03 == 0x03 {   // interrupt endpoint
            if address & 0x80 != 0 { inAddress = inAddress ?? address } else { outAddress = outAddress ?? address }
        }
        current = IOUSBGetNextEndpointDescriptor(configuration, interfaceDescriptor,
                                                 UnsafeRawPointer(endpoint).assumingMemoryBound(to: IOUSBDescriptorHeader.self))
    }
    guard let inAddress, let outAddress else { return nil }
    return (inAddress, outAddress)
}

// MARK: - Main

let options = Options(arguments: CommandLine.arguments)

guard let receiverService = findReceiver() else {
    print("No Xbox 360 Wireless Receiver found.")
    exit(1)
}
print("Found receiver \(String(format: "%04X:%04X", intProperty(receiverService, "idVendor") ?? 0, intProperty(receiverService, "idProduct") ?? 0))")

let queue = DispatchQueue(label: "x360probe")
let device: IOUSBHostDevice
do {
    device = try IOUSBHostDevice(__ioService: receiverService, options: [], queue: queue, interestHandler: nil)
} catch {
    print("Could not open the receiver (\(error)). Another process or driver may own it exclusively.")
    exit(1)
}

if device.configurationDescriptor == nil {
    print("Receiver is unconfigured — selecting configuration 1")
    do {
        try device.__configure(withValue: 1, matchInterfaces: true)
    } catch {
        print("SetConfiguration failed: \(error)")
        device.destroy()
        exit(1)
    }
}

// Interfaces are published asynchronously after SetConfiguration.
var interfaceServices: [io_service_t] = []
for _ in 0..<40 {
    interfaceServices = controllerInterfaces(of: receiverService)
    if !interfaceServices.isEmpty { break }
    Thread.sleep(forTimeInterval: 0.05)
}
print("Controller interfaces: \(interfaceServices.count)")

var slots: [Slot] = []
for (index, service) in interfaceServices.enumerated() {
    do {
        let interface = try IOUSBHostInterface(__ioService: service, options: [], queue: queue, interestHandler: nil)
        guard let addresses = endpointAddresses(of: interface) else {
            print("[slot \(index)] no interrupt endpoints")
            continue
        }
        let inPipe = try interface.copyPipe(withAddress: Int(addresses.in))
        let outPipe = try interface.copyPipe(withAddress: Int(addresses.out))
        print("[slot \(index)] IN 0x\(String(addresses.in, radix: 16)) OUT 0x\(String(addresses.out, radix: 16))")
        slots.append(Slot(index: index, interface: interface, inPipe: inPipe, outPipe: outPipe, options: options))
    } catch {
        print("[slot \(index)] open failed: \(error)")
    }
}

guard !slots.isEmpty else {
    print("No usable slots.")
    device.destroy()
    exit(1)
}

print("Listening for \(Int(options.seconds)) s — press buttons, move sticks, or press the guide button to (re)connect a pad.")
queue.sync { slots.forEach { $0.start() } }
Thread.sleep(forTimeInterval: options.seconds)

queue.sync {
    for slot in slots {
        try? slot.inPipe.__abort(with: .synchronous)
        slot.interface.destroy()
    }
    device.destroy()
}
print("Done.")
