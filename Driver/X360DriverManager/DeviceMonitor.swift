//
//  DeviceMonitor.swift
//  X360DriverManager
//
//  Polls the I/O Registry for the receiver and the gamepads the driver publishes.
//

import Foundation
import IOKit
import Observation

struct PublishedGamepad: Identifiable, Equatable {
    let id: UInt64          // Registry entry ID
    let name: String
    let serial: String
    /// 0–255 as reported by the controller, nil until the pad has announced it.
    let batteryLevel: Int?
}

@MainActor
@Observable
final class DeviceMonitor {
    private(set) var receiverConnected = false
    /// True when our driver (rather than nothing or another driver) owns the receiver.
    private(set) var driverAttached = false
    private(set) var gamepads: [PublishedGamepad] = []

    private static let receiverProductIDs: Set<Int> = [0x0719, 0x0291, 0x02A9]

    /// Refreshes every `interval` until the calling task is cancelled (use with `.task`).
    func run(interval: Duration = .seconds(1)) async {
        while !Task.isCancelled {
            refresh()
            try? await Task.sleep(for: interval)
        }
    }

    func refresh() {
        let receivers = Self.services(matching: IOServiceMatching("IOUSBHostDevice")).filter { service in
            Self.intProperty(service, "idVendor") == 0x045E
                && Self.receiverProductIDs.contains(Self.intProperty(service, "idProduct") ?? -1)
        }
        receiverConnected = !receivers.isEmpty
        driverAttached = receivers.contains { Self.hasChild(of: $0, userClass: "X360WirelessReceiver") }
        receivers.forEach { IOObjectRelease($0) }

        let pads = Self.services(matching: IOServiceMatching("IOHIDDevice"))
        gamepads = pads.compactMap { service in
            guard Self.stringProperty(service, "IOUserClass") == "X360WirelessGamepad" else { return nil }
            var entryID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(service, &entryID)
            return PublishedGamepad(id: entryID,
                                    name: Self.stringProperty(service, "Product") ?? "Gamepad",
                                    serial: Self.stringProperty(service, "SerialNumber") ?? "",
                                    batteryLevel: Self.intProperty(service, "BatteryLevel"))
        }
        .sorted { $0.serial < $1.serial }
        pads.forEach { IOObjectRelease($0) }
    }

    // MARK: Registry helpers

    private static func services(matching dictionary: CFMutableDictionary?) -> [io_service_t] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, dictionary, &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var result: [io_service_t] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            result.append(service)
        }
        return result
    }

    private static func hasChild(of service: io_service_t, userClass: String) -> Bool {
        var iterator: io_iterator_t = 0
        guard IORegistryEntryGetChildIterator(service, kIOServicePlane, &iterator) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(iterator) }
        var found = false
        while case let child = IOIteratorNext(iterator), child != 0 {
            if stringProperty(child, "IOUserClass") == userClass { found = true }
            IOObjectRelease(child)
        }
        return found
    }

    private static func property(_ service: io_service_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    private static func intProperty(_ service: io_service_t, _ key: String) -> Int? {
        property(service, key) as? Int
    }

    private static func stringProperty(_ service: io_service_t, _ key: String) -> String? {
        property(service, key) as? String
    }
}
