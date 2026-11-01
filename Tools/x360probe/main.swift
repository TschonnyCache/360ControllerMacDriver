//
//  x360probe — talks to an Xbox 360 Wireless Receiver (or a wired Xbox 360
//  controller) from user space — no driver, no signing — to validate the
//  protocol implementation in Core/.
//
//  Usage: x360probe [--seconds N] [--rumble] [--led N] [--power-off] [--raw]
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
    /// LED pattern (0–13) to show instead of the player number.
    var led: UInt8?
    /// Switch wireless pads off as soon as they are detected.
    var powerOff = false

    static let usage = """
        usage: x360probe [--seconds N] [--rumble] [--led N] [--power-off] [--raw]
          --seconds N   listen for N seconds (default 20)
          --rumble      short rumble when a pad is detected
          --led N       show LED pattern N (0 off, 1 blink all, 2–5 flash+player, 6–9 player,
                        10 rotate, 11 blink, 12 slow blink, 13 alternate)
          --power-off   switch wireless pads off when detected
          --raw         print every packet
        """

    init(arguments: [String]) {
        var iterator = arguments.dropFirst().makeIterator()
        while let argument = iterator.next() {
            switch argument {
            case "--seconds": seconds = iterator.next().flatMap(Double.init) ?? seconds
            case "--rumble": rumble = true
            case "--raw": raw = true
            case "--power-off": powerOff = true
            case "--led":
                guard let value = iterator.next().flatMap(UInt8.init), value <= 13 else {
                    print(Self.usage)
                    exit(64)
                }
                led = value
            default:
                print(Self.usage)
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
let wiredProductIDs: Set<Int> = [
    Int(X360_PRODUCT_WIRED_CONTROLLER),
    Int(X360_PRODUCT_WIRED_CONTROLLER_2),
]

// MARK: - Registry helpers

func intProperty(_ service: io_service_t, _ key: String) -> Int? {
    IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
        .takeRetainedValue() as? Int
}

/// The first receiver, or else the first wired pad.
func findDevice() -> (service: io_service_t, wired: Bool)? {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator) == KERN_SUCCESS else {
        return nil
    }
    defer { IOObjectRelease(iterator) }
    var wired: io_service_t?
    while case let service = IOIteratorNext(iterator), service != 0 {
        if intProperty(service, "idVendor") == Int(X360_VENDOR_MICROSOFT), let product = intProperty(service, "idProduct") {
            if receiverProductIDs.contains(product) {
                if let wired { IOObjectRelease(wired) }
                return (service, false)
            }
            if wiredProductIDs.contains(product), wired == nil {
                wired = service
                continue
            }
        }
        IOObjectRelease(service)
    }
    return wired.map { ($0, true) }
}

/// Controller interfaces that are children of `device`, sorted by interface number:
/// protocol 0x81 on a receiver, subclass 0x5D / protocol 0x01 on a wired pad.
func controllerInterfaces(of device: io_service_t, wired: Bool) -> [io_service_t] {
    var iterator: io_iterator_t = 0
    guard IORegistryEntryGetChildIterator(device, kIOServicePlane, &iterator) == KERN_SUCCESS else { return [] }
    defer { IOObjectRelease(iterator) }
    var found: [(Int, io_service_t)] = []
    while case let child = IOIteratorNext(iterator), child != 0 {
        let matches = wired
            ? intProperty(child, "bInterfaceSubClass") == Int(X360_WIRED_INTERFACE_SUBCLASS)
                && intProperty(child, "bInterfaceProtocol") == Int(X360_WIRED_INTERFACE_PROTOCOL)
            : intProperty(child, "bInterfaceProtocol") == Int(X360_INTERFACE_PROTOCOL_CONTROLLER)
        if IOObjectConformsTo(child, "IOUSBHostInterface") != 0, matches {
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

/// One controller slot of the receiver, or the wired pad. All mutable state is confined to the shared serial queue.
final class Slot: @unchecked Sendable {
    let index: Int
    let wired: Bool
    let interface: IOUSBHostInterface
    let inPipe: IOUSBHostPipe
    let outPipe: IOUSBHostPipe
    let options: Options
    var lastState: X360PadState?
    var connected = false

    init(index: Int, wired: Bool, interface: IOUSBHostInterface, inPipe: IOUSBHostPipe, outPipe: IOUSBHostPipe,
         options: Options) {
        self.index = index
        self.wired = wired
        self.interface = interface
        self.inPipe = inPipe
        self.outPipe = outPipe
        self.options = options
    }

    func log(_ message: String) {
        print(wired ? "[wired] \(message)" : "[slot \(index)] \(message)")
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
        if wired {
            log("controller present")
            becomeActive()
        } else {
            send({ X360BuildInquiryCommand($0, $1) }, "inquiry")
        }
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
        let parsed = packet.withUnsafeBufferPointer {
            wired ? X360ParseWiredPacket($0.baseAddress, $0.count, &event)
                  : X360ParseWirelessPacket($0.baseAddress, $0.count, &event)
        }
        guard parsed else {
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
        if options.powerOff {
            if wired {
                log("wired pads can't be switched off")
            } else {
                log("switching off")
                send({ X360BuildPowerOffCommand($0, $1) }, "power off")
                return
            }
        }
        let pattern = options.led ?? UInt8(X360LEDPatternForSlot(UInt32(index)).rawValue)
        if wired {
            send({ X360BuildWiredLEDCommand(pattern, $0, $1) }, "LED")
        } else {
            send({ X360BuildLEDCommand(pattern, $0, $1) }, "LED")
            send({ X360BuildEnableReportingCommand($0, $1) }, "enable")
        }
        if options.rumble {
            rumble(0x80)
            interface.queue.asyncAfter(deadline: .now() + 0.4) { [self] in rumble(0) }
        }
    }

    private func rumble(_ level: UInt8) {
        if wired {
            send({ X360BuildWiredRumbleCommand(level, level, $0, $1) }, "rumble")
        } else {
            send({ X360BuildRumbleCommand(level, level, $0, $1) }, "rumble")
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

guard let (receiverService, isWired) = findDevice() else {
    print("No Xbox 360 Wireless Receiver or wired Xbox 360 controller found.")
    exit(1)
}
print("Found \(isWired ? "wired controller" : "receiver") \(String(format: "%04X:%04X", intProperty(receiverService, "idVendor") ?? 0, intProperty(receiverService, "idProduct") ?? 0))")

let queue = DispatchQueue(label: "x360probe")
let device: IOUSBHostDevice
do {
    device = try IOUSBHostDevice(__ioService: receiverService, options: [], queue: queue, interestHandler: nil)
} catch {
    print("Could not open the device (\(error)). Another process or driver (X360Driver?) may own it exclusively.")
    exit(1)
}

if device.configurationDescriptor == nil {
    print("Device is unconfigured — selecting configuration 1")
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
    interfaceServices = controllerInterfaces(of: receiverService, wired: isWired)
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
        slots.append(Slot(index: index, wired: isWired, interface: interface, inPipe: inPipe, outPipe: outPipe,
                          options: options))
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
