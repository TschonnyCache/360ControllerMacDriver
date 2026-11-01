//
//  DeviceMonitor.swift
//  X360DriverManager
//
//  Polls the I/O Registry for supported USB hardware and whether X360Driver
//  has attached to it. (Live controller state comes from GamepadMonitor.)
//

import Foundation
import IOKit
import Observation

@MainActor
@Observable
final class DeviceMonitor {
    struct Hardware: Identifiable, Equatable {
        enum Kind { case receiver, wiredController }
        let id: UInt64          // Registry entry ID
        let kind: Kind
        /// True when our driver (rather than nothing or another driver) owns the device.
        let driverAttached: Bool
    }

    private(set) var hardware: [Hardware] = []

    var receiverConnected: Bool { hardware.contains { $0.kind == .receiver } }
    var wiredControllerCount: Int { hardware.filter { $0.kind == .wiredController }.count }
    var driverAttached: Bool { !hardware.isEmpty && hardware.allSatisfy(\.driverAttached) }

    private static let microsoft = 0x045E
    private static let kinds: [Int: (Hardware.Kind, String)] = [
        0x0719: (.receiver, "X360WirelessReceiver"),
        0x0291: (.receiver, "X360WirelessReceiver"),
        0x02A9: (.receiver, "X360WirelessReceiver"),
        0x028E: (.wiredController, "X360WiredController"),
        0x028F: (.wiredController, "X360WiredController"),
    ]

    /// Refreshes every `interval` until the calling task is cancelled (use with `.task`).
    func run(interval: Duration = .seconds(1)) async {
        while !Task.isCancelled {
            refresh()
            try? await Task.sleep(for: interval)
        }
    }

    func refresh() {
        var found: [Hardware] = []
        for service in Self.services(matching: IOServiceMatching("IOUSBHostDevice")) {
            defer { IOObjectRelease(service) }
            guard Self.intProperty(service, "idVendor") == Self.microsoft,
                  let product = Self.intProperty(service, "idProduct"),
                  let (kind, userClass) = Self.kinds[product] else { continue }
            var entryID: UInt64 = 0
            IORegistryEntryGetRegistryEntryID(service, &entryID)
            found.append(Hardware(id: entryID, kind: kind,
                                  driverAttached: Self.hasChild(of: service, userClass: userClass)))
        }
        if found != hardware {
            hardware = found
        }
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
