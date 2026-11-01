//
//  GamepadMonitor.swift
//  X360DriverManager
//
//  Watches the HID game pads published by X360Driver through IOHIDManager —
//  the same path games use — so the app can show live input and send rumble /
//  LED output reports. If input shows up here, games will see it too.
//

import Foundation
import IOKit.hid
import Observation

/// Controls of an Xbox 360 pad, with their HID button numbers (360Controller layout).
enum PadButton: Int, CaseIterable, Identifiable {
    case a = 1, b, x, y, leftBumper, rightBumper, leftStick, rightStick, start, back, guide
    case dpadUp, dpadDown, dpadLeft, dpadRight

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .a: "A"
        case .b: "B"
        case .x: "X"
        case .y: "Y"
        case .leftBumper: "LB"
        case .rightBumper: "RB"
        case .leftStick: "LS"
        case .rightStick: "RS"
        case .start: "Start"
        case .back: "Back"
        case .guide: "Guide"
        case .dpadUp: "↑"
        case .dpadDown: "↓"
        case .dpadLeft: "←"
        case .dpadRight: "→"
        }
    }
}

/// Live state of one published game pad.
@MainActor
@Observable
final class ConnectedGamepad: Identifiable {
    let id: UInt64              // Registry entry ID of the HID device
    let name: String
    let isWired: Bool
    /// Receiver slot (0–3) parsed from the serial number; 0 for wired pads.
    let slot: Int
    fileprivate let device: IOHIDDevice

    private(set) var pressed: Set<PadButton> = []
    /// Stick positions in -1…1, y positive = up.
    private(set) var leftStick = CGPoint.zero
    private(set) var rightStick = CGPoint.zero
    /// Trigger positions in 0…1.
    private(set) var leftTrigger = 0.0
    private(set) var rightTrigger = 0.0
    /// 0–255 as reported by the controller, nil until known.
    private(set) var batteryLevel: Int?

    fileprivate init(id: UInt64, device: IOHIDDevice) {
        self.id = id
        self.device = device
        name = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "Gamepad"
        isWired = (IOHIDDeviceGetProperty(device, kIOHIDTransportKey as CFString) as? String) == "USB"
        let serial = IOHIDDeviceGetProperty(device, kIOHIDSerialNumberKey as CFString) as? String ?? ""
        // The driver names wireless pads "X360-SLOT-<1…4>".
        slot = serial.last.flatMap { Int(String($0)) }.map { max($0 - 1, 0) } ?? 0
        refreshBattery()
    }

    fileprivate func apply(_ value: IOHIDValue) {
        let element = IOHIDValueGetElement(value)
        let page = IOHIDElementGetUsagePage(element)
        let usage = IOHIDElementGetUsage(element)
        let raw = IOHIDValueGetIntegerValue(value)
        // Sticks: -32768…32767, HID convention y positive = down.
        let axis = { (v: Int) in max(-1, min(1, Double(v) / 32767)) }

        switch (Int(page), Int(usage)) {
        case (kHIDPage_Button, let number):
            guard let button = PadButton(rawValue: number) else { return }
            if raw != 0 { pressed.insert(button) } else { pressed.remove(button) }
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_X): leftStick.x = axis(raw)
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_Y): leftStick.y = -axis(raw)
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_Rx): rightStick.x = axis(raw)
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_Ry): rightStick.y = -axis(raw)
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_Z): leftTrigger = Double(raw) / 255
        case (kHIDPage_GenericDesktop, kHIDUsage_GD_Rz): rightTrigger = Double(raw) / 255
        default: break
        }
    }

    func refreshBattery() {
        batteryLevel = IOHIDDeviceGetProperty(device, "BatteryLevel" as CFString) as? Int
    }

    // MARK: Output (360Controller report format, understood by X360Driver)

    /// Runs both motors at `strength` (0…1) for `duration`.
    func rumble(strength: Double = 0.6, for duration: Duration = .milliseconds(400)) async {
        let level = UInt8(max(0, min(1, strength)) * 255)
        send([0x00, 0x04, level, level])
        try? await Task.sleep(for: duration)
        send([0x00, 0x04, 0x00, 0x00])
    }

    /// Blinks the ring so the person can tell which physical pad this is, then restores the player light.
    func identify() async {
        send([0x01, 0x03, 0x0D])                   // Alternating pattern
        try? await Task.sleep(for: .seconds(2))
        send([0x01, 0x03, UInt8(6 + slot)])        // Solid player light (6 = player 1)
    }

    private func send(_ bytes: [UInt8]) {
        let result = bytes.withUnsafeBufferPointer { buffer in
            IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0, buffer.baseAddress!, buffer.count)
        }
        if result != kIOReturnSuccess {
            print("X360DriverManager: output report failed 0x\(String(UInt32(bitPattern: result), radix: 16))")
        }
    }
}

@MainActor
@Observable
final class GamepadMonitor {
    private(set) var gamepads: [ConnectedGamepad] = []
    private(set) var isRunning = false

    @ObservationIgnored private var manager: IOHIDManager?

    /// Starts watching; keeps battery levels fresh until the calling task is cancelled.
    func run() async {
        start()
        defer { stop() }
        while !Task.isCancelled {
            gamepads.forEach { $0.refreshBattery() }
            try? await Task.sleep(for: .seconds(5))
        }
    }

    private func start() {
        guard manager == nil else { return }
        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        IOHIDManagerSetDeviceMatching(manager, [
            kIOHIDVendorIDKey: 0x045E,
            kIOHIDProductIDKey: 0x028E,
            kIOHIDPrimaryUsagePageKey: kHIDPage_GenericDesktop,
            kIOHIDPrimaryUsageKey: kHIDUsage_GD_GamePad,
        ] as CFDictionary)

        let context = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, _, _, device in
            guard let context else { return }
            let monitor = Unmanaged<GamepadMonitor>.fromOpaque(context).takeUnretainedValue()
            MainActor.assumeIsolated { monitor.deviceAdded(device) }
        }, context)
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, _, _, device in
            guard let context else { return }
            let monitor = Unmanaged<GamepadMonitor>.fromOpaque(context).takeUnretainedValue()
            MainActor.assumeIsolated { monitor.deviceRemoved(device) }
        }, context)
        IOHIDManagerRegisterInputValueCallback(manager, { context, _, _, value in
            guard let context else { return }
            let monitor = Unmanaged<GamepadMonitor>.fromOpaque(context).takeUnretainedValue()
            MainActor.assumeIsolated { monitor.valueChanged(value) }
        }, context)

        // Callbacks arrive on the main run loop, i.e. on the main actor.
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let result = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        isRunning = result == kIOReturnSuccess
        self.manager = manager
    }

    private func stop() {
        guard let manager else { return }
        IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        self.manager = nil
        gamepads = []
        isRunning = false
    }

    private static func registryID(of device: IOHIDDevice) -> UInt64 {
        var id: UInt64 = 0
        IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device), &id)
        return id
    }

    private func deviceAdded(_ device: IOHIDDevice) {
        // Only our driver's pads (a real wired 360 pad on another driver has the same VID/PID).
        guard IOHIDDeviceGetProperty(device, "IOUserClass" as CFString) as? String == "X360Gamepad" else { return }
        let id = Self.registryID(of: device)
        guard !gamepads.contains(where: { $0.id == id }) else { return }
        gamepads.append(ConnectedGamepad(id: id, device: device))
        gamepads.sort { ($0.isWired ? 10 : 0) + $0.slot < ($1.isWired ? 10 : 0) + $1.slot }
    }

    private func deviceRemoved(_ device: IOHIDDevice) {
        let id = Self.registryID(of: device)
        gamepads.removeAll { $0.id == id }
    }

    private func valueChanged(_ value: IOHIDValue) {
        let device = IOHIDElementGetDevice(IOHIDValueGetElement(value))
        let id = Self.registryID(of: device)
        gamepads.first { $0.id == id }?.apply(value)
    }
}
