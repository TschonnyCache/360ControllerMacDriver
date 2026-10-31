import Testing
import X360Protocol

/// Parses `bytes` as a wireless packet and returns the event.
private func parse(_ bytes: [UInt8]) -> (ok: Bool, event: X360Event) {
    var event = X360Event()
    let ok = bytes.withUnsafeBufferPointer { X360ParseWirelessPacket($0.baseAddress, $0.count, &event) }
    return (ok, event)
}

/// A 29-byte wireless input packet wrapping the given 360 report fields.
private func inputPacket(buttons: UInt16, lt: UInt8, rt: UInt8,
                         lx: Int16, ly: Int16, rx: Int16, ry: Int16) -> [UInt8] {
    var packet: [UInt8] = [0x00, 0x01, 0x00, 0xF0, 0x00, 0x13]
    packet += [UInt8(buttons & 0xFF), UInt8(buttons >> 8), lt, rt]
    for axis in [lx, ly, rx, ry] {
        let raw = UInt16(bitPattern: axis)
        packet += [UInt8(raw & 0xFF), UInt8(raw >> 8)]
    }
    packet += [UInt8](repeating: 0, count: 29 - packet.count)
    return packet
}

private func command(_ build: (UnsafeMutablePointer<UInt8>, Int) -> Int) -> [UInt8] {
    var buffer = [UInt8](repeating: 0xEE, count: Int(X360_WIRELESS_COMMAND_SIZE))
    let written = buffer.withUnsafeMutableBufferPointer { build($0.baseAddress!, $0.count) }
    return Array(buffer.prefix(written))
}

@Suite("Receiver → host packets")
struct WirelessPacketTests {
    @Test func controllerConnected() {
        let (ok, event) = parse([0x08, 0x80])
        #expect(ok)
        #expect(event.type == X360_EVENT_CONNECTED)
        #expect(!event.headset)
    }

    @Test func controllerAndHeadsetConnected() {
        let (_, event) = parse([0x08, 0xC0])
        #expect(event.type == X360_EVENT_CONNECTED)
        #expect(event.headset)
    }

    @Test(arguments: [[UInt8]([0x08, 0x00]), [0x08, 0x40]])
    func slotWithoutControllerIsDisconnected(packet: [UInt8]) {
        #expect(parse(packet).event.type == X360_EVENT_DISCONNECTED)
    }

    @Test func inputReport() {
        let buttons = UInt16(X360_BUTTON_A | X360_BUTTON_DPAD_LEFT | X360_BUTTON_GUIDE)
        let (ok, event) = parse(inputPacket(buttons: buttons, lt: 0x40, rt: 0xFF,
                                            lx: -32768, ly: 32767, rx: 1234, ry: -1))
        #expect(ok)
        #expect(event.type == X360_EVENT_INPUT)
        #expect(event.state.buttons == buttons)
        #expect(event.state.leftTrigger == 0x40)
        #expect(event.state.rightTrigger == 0xFF)
        #expect(event.state.leftX == -32768)
        #expect(event.state.leftY == 32767)
        #expect(event.state.rightX == 1234)
        #expect(event.state.rightY == -1)
    }

    @Test func truncatedInputIsRejected() {
        let packet = Array(inputPacket(buttons: 1, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0).prefix(17))
        let (ok, event) = parse(packet)
        #expect(!ok)
        #expect(event.type == X360_EVENT_NONE)
    }

    @Test func announcementCarriesSerialAndBattery() {
        var packet = [UInt8](repeating: 0, count: 29)
        packet[1] = 0x0F
        packet[10...13] = [0xDE, 0xAD, 0xBE, 0xEF]
        packet[16] = 0x13
        packet[17] = 0xA0
        let (ok, event) = parse(packet)
        #expect(ok)
        #expect(event.type == X360_EVENT_INFO)
        #expect(event.hasBattery)
        #expect(event.battery == 0xA0)
        #expect([event.serial.0, event.serial.1, event.serial.2, event.serial.3] == [0xDE, 0xAD, 0xBE, 0xEF])
    }

    @Test func batteryStatus() {
        var packet = [UInt8](repeating: 0, count: 29)
        packet[3] = 0x13
        packet[4] = 0x7F
        let (ok, event) = parse(packet)
        #expect(ok)
        #expect(event.type == X360_EVENT_BATTERY)
        #expect(event.battery == 0x7F)
    }

    @Test func idleTrafficIsIgnored() {
        // Receivers periodically send all-zero keep-alive packets.
        #expect(!parse([UInt8](repeating: 0, count: 29)).ok)
        #expect(!parse([]).ok)
    }
}

@Suite("Host → receiver commands")
struct CommandTests {
    @Test func led() {
        #expect(command { X360BuildLEDCommand(UInt8(X360_LED_ON_2.rawValue), $0, $1) }
                == [0x00, 0x00, 0x08, 0x47, 0, 0, 0, 0, 0, 0, 0, 0])
    }

    @Test func ledPatternPerSlot() {
        #expect(X360LEDPatternForSlot(0) == X360_LED_FLASH_THEN_ON_1)
        #expect(X360LEDPatternForSlot(3) == X360_LED_FLASH_THEN_ON_4)
    }

    @Test func rumble() {
        #expect(command { X360BuildRumbleCommand(0xAA, 0x55, $0, $1) }
                == [0x00, 0x01, 0x0F, 0xC0, 0x00, 0xAA, 0x55, 0, 0, 0, 0, 0])
    }

    @Test func powerOff() {
        #expect(command { X360BuildPowerOffCommand($0, $1) }
                == [0x00, 0x00, 0x08, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0])
    }

    @Test func inquiry() {
        #expect(command { X360BuildInquiryCommand($0, $1) }
                == [0x08, 0x00, 0x0F, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0])
    }

    @Test func tooSmallBufferWritesNothing() {
        var small = [UInt8](repeating: 0, count: 4)
        #expect(small.withUnsafeMutableBufferPointer { X360BuildRumbleCommand(1, 1, $0.baseAddress, $0.count) } == 0)
    }
}

@Suite("Virtual HID gamepad")
struct HIDTests {
    /// Walks the report descriptor and returns the total number of input bits,
    /// so we know the report we emit actually matches what we declare.
    private func declaredInputBits() -> Int {
        let descriptor = Array(UnsafeBufferPointer(start: X360HIDReportDescriptorBytes(),
                                                   count: X360HIDReportDescriptorSize))
        var index = 0, reportSize = 0, reportCount = 0, inputBits = 0, depth = 0
        while index < descriptor.count {
            let prefix = descriptor[index]
            let size = [0, 1, 2, 4][Int(prefix & 0x03)]
            var value = 0
            for byte in 0..<size { value |= Int(descriptor[index + 1 + byte]) << (8 * byte) }
            switch prefix & 0xFC {
            case 0x74: reportSize = value
            case 0x94: reportCount = value
            case 0x80: inputBits += reportSize * reportCount
            case 0xA0: depth += 1
            case 0xC0: depth -= 1
            default: break
            }
            index += 1 + size
        }
        #expect(depth == 0, "collections must be balanced")
        return inputBits
    }

    @Test func descriptorDescribesFourteenBytes() {
        #expect(declaredInputBits() == 14 * 8)
    }

    @Test func reportMatchesWiredLayoutWithInvertedY() {
        var state = X360PadState()
        state.buttons = UInt16(X360_BUTTON_Y | X360_BUTTON_START)
        state.leftTrigger = 1
        state.rightTrigger = 2
        state.leftX = 100
        state.leftY = 32767     // pushed fully up
        state.rightX = -32768
        state.rightY = -32768   // pulled fully down

        var report = [UInt8](repeating: 0xEE, count: Int(X360_HID_REPORT_SIZE))
        let written = report.withUnsafeMutableBufferPointer { X360BuildHIDReport(&state, $0.baseAddress, $0.count) }
        #expect(written == Int(X360_HID_REPORT_SIZE))
        #expect(Array(report[0...5]) == [0x00, 0x14, 0x10, 0x80, 0x01, 0x02])
        #expect(Array(report[6...7]) == [0x64, 0x00])            // LX = 100
        #expect(Array(report[8...9]) == [0x00, 0x80])            // ~32767  = -32768 (up → negative)
        #expect(Array(report[10...11]) == [0x00, 0x80])          // RX = -32768
        #expect(Array(report[12...13]) == [0xFF, 0x7F])          // ~-32768 = 32767 (down → positive)
        #expect(report[14...].allSatisfy { $0 == 0 })
    }

    @Test func outputReports() {
        func decode(_ bytes: [UInt8]) -> X360OutputRequest {
            var request = X360OutputRequest()
            _ = bytes.withUnsafeBufferPointer { X360ParseOutputReport($0.baseAddress, $0.count, &request) }
            return request
        }
        let rumble = decode([0x00, 0x04, 0x80, 0x20])
        #expect(rumble.type == X360_OUTPUT_RUMBLE && rumble.large == 0x80 && rumble.small == 0x20)
        let led = decode([0x01, 0x03, 0x07])
        #expect(led.type == X360_OUTPUT_LED && led.pattern == 7)
        #expect(decode([0x02, 0x00]).type == X360_OUTPUT_POWER_OFF)
        #expect(decode([0x00, 0x05, 0, 0]).type == X360_OUTPUT_UNSUPPORTED)
    }
}
