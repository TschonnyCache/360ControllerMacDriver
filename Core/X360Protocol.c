//
//  X360Protocol.c
//  See X360Protocol.h. Deliberately freestanding (no libc calls) so it can be
//  compiled unchanged for DriverKit, macOS user space and tests.
//

#include "X360Protocol.h"

// MARK: - Helpers

static void Clear(void *memory, size_t length)
{
    uint8_t *bytes = (uint8_t *)memory;
    for (size_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static uint16_t ReadLE16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void WriteLE16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)(value >> 8);
}

/// Starts a 12-byte command buffer. Returns false if it doesn't fit.
static bool BeginCommand(uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < X360_WIRELESS_COMMAND_SIZE) {
        return false;
    }
    Clear(out, X360_WIRELESS_COMMAND_SIZE);
    return true;
}

// MARK: - Receiver → host

// Byte offsets inside a wireless packet.
enum {
    kPacketKind          = 0,    // 0x08 = connection status, 0x00 = data
    kPacketSubtype       = 1,
    kDataMarker          = 3,    // 0xF0 for input reports
    kInputReportOffset   = 4,    // Embedded 360 input report starts here
    kInfoSerialOffset    = 10,   // 4 bytes of serial in announcement packets
    kInfoBatteryTag      = 16,   // 0x13 if the announcement carries battery level
    kInfoBatteryValue    = 17,
    kStatusBatteryValue  = 4,
};

// Byte offsets inside a 360 input report.
enum {
    kReportButtons       = 2,
    kReportLeftTrigger   = 4,
    kReportRightTrigger  = 5,
    kReportLeftX         = 6,
    kReportLeftY         = 8,
    kReportRightX        = 10,
    kReportRightY        = 12,
    kReportMinimumLength = 14,
};

bool X360ParseInputReport(const uint8_t *report, size_t length, X360PadState *out)
{
    if (report == NULL || out == NULL || length < kReportMinimumLength || report[0] != 0x00) {
        return false;
    }
    out->buttons      = ReadLE16(report + kReportButtons);
    out->leftTrigger  = report[kReportLeftTrigger];
    out->rightTrigger = report[kReportRightTrigger];
    out->leftX        = (int16_t)ReadLE16(report + kReportLeftX);
    out->leftY        = (int16_t)ReadLE16(report + kReportLeftY);
    out->rightX       = (int16_t)ReadLE16(report + kReportRightX);
    out->rightY       = (int16_t)ReadLE16(report + kReportRightY);
    return true;
}

bool X360ParseWirelessPacket(const uint8_t *data, size_t length, X360Event *out)
{
    if (out == NULL) {
        return false;
    }
    Clear(out, sizeof(*out));
    if (data == NULL || length < 2) {
        return false;
    }

    // Connection status: `08 xx`. Bit 7 = controller present, bit 6 = headset present.
    if (data[kPacketKind] == 0x08 && length == 2) {
        if (data[kPacketSubtype] & 0x80) {
            out->type = X360_EVENT_CONNECTED;
            out->headset = (data[kPacketSubtype] & 0x40) != 0;
        } else {
            out->type = X360_EVENT_DISCONNECTED;
        }
        return true;
    }

    if (data[kPacketKind] != 0x00) {
        return false;
    }

    switch (data[kPacketSubtype]) {
        case 0x01:
            // Input: `00 01 00 F0 | 00 13 <360 input report…>`
            if (length >= kInputReportOffset + kReportMinimumLength && data[kDataMarker] == 0xF0
                && X360ParseInputReport(data + kInputReportOffset, length - kInputReportOffset, &out->state)) {
                out->type = X360_EVENT_INPUT;
                return true;
            }
            break;

        case 0x0F:
            // Announcement, sent when a controller links: carries serial and usually battery.
            if (length > kInfoBatteryValue) {
                for (int i = 0; i < 4; i++) {
                    out->serial[i] = data[kInfoSerialOffset + i];
                }
                if (data[kInfoBatteryTag] == 0x13) {
                    out->battery = data[kInfoBatteryValue];
                    out->hasBattery = true;
                }
                out->type = X360_EVENT_INFO;
                return true;
            }
            break;

        case 0x00:
            // Status update: `00 00 00 13 <level>` is a battery report.
            if (length > kStatusBatteryValue && data[kDataMarker] == 0x13) {
                out->type = X360_EVENT_BATTERY;
                out->battery = data[kStatusBatteryValue];
                out->hasBattery = true;
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

bool X360ParseWiredPacket(const uint8_t *data, size_t length, X360Event *out)
{
    if (out == NULL) {
        return false;
    }
    Clear(out, sizeof(*out));
    // Input report: `00 14 <buttons…>`. Other types (01 = LED status, 03 = rumble
    // status, 08 = headset) carry nothing we use.
    if (data != NULL && length >= 2 && data[0] == 0x00 && data[1] == 0x14
        && X360ParseInputReport(data, length, &out->state)) {
        out->type = X360_EVENT_INPUT;
        return true;
    }
    return false;
}

// MARK: - Host → receiver

X360LEDPattern X360LEDPatternForSlot(unsigned slot)
{
    return (X360LEDPattern)(X360_LED_FLASH_THEN_ON_1 + (slot % X360_MAX_SLOTS));
}

size_t X360BuildLEDCommand(uint8_t pattern, uint8_t *out, size_t capacity)
{
    if (!BeginCommand(out, capacity)) {
        return 0;
    }
    out[2] = 0x08;
    out[3] = (uint8_t)(0x40 + (pattern % 0x0E));
    return X360_WIRELESS_COMMAND_SIZE;
}

size_t X360BuildRumbleCommand(uint8_t large, uint8_t small, uint8_t *out, size_t capacity)
{
    if (!BeginCommand(out, capacity)) {
        return 0;
    }
    out[1] = 0x01;
    out[2] = 0x0F;
    out[3] = 0xC0;
    out[5] = large;
    out[6] = small;
    return X360_WIRELESS_COMMAND_SIZE;
}

size_t X360BuildPowerOffCommand(uint8_t *out, size_t capacity)
{
    if (!BeginCommand(out, capacity)) {
        return 0;
    }
    out[2] = 0x08;
    out[3] = 0xC0;
    return X360_WIRELESS_COMMAND_SIZE;
}

size_t X360BuildEnableReportingCommand(uint8_t *out, size_t capacity)
{
    if (!BeginCommand(out, capacity)) {
        return 0;
    }
    out[3] = 0x40;
    return X360_WIRELESS_COMMAND_SIZE;
}

size_t X360BuildInquiryCommand(uint8_t *out, size_t capacity)
{
    if (!BeginCommand(out, capacity)) {
        return 0;
    }
    out[0] = 0x08;
    out[2] = 0x0F;
    out[3] = 0xC0;
    return X360_WIRELESS_COMMAND_SIZE;
}

// MARK: - Host → wired controller

size_t X360BuildWiredLEDCommand(uint8_t pattern, uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < 3) {
        return 0;
    }
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = (uint8_t)(pattern % 0x0E);
    return 3;
}

size_t X360BuildWiredRumbleCommand(uint8_t large, uint8_t small, uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < 8) {
        return 0;
    }
    Clear(out, 8);
    out[1] = 0x08;
    out[3] = large;
    out[4] = small;
    return 8;
}

// MARK: - Virtual HID gamepad

// Byte-for-byte the descriptor from 360Controller's xbox360hid.h.
const uint8_t X360HIDReportDescriptor[] = {
    0x05, 0x01,             // USAGE_PAGE (Generic Desktop)
    0x09, 0x05,             // USAGE (Game Pad)
    0xA1, 0x01,             // COLLECTION (Application)
    0x05, 0x01,             //   USAGE_PAGE (Generic Desktop)
    0x09, 0x3A,             //   USAGE (Counted Buffer)
    0xA1, 0x02,             //   COLLECTION (Logical)
    0x75, 0x08,             //     REPORT_SIZE (8)
    0x95, 0x02,             //     REPORT_COUNT (2)
    0x05, 0x01,             //     USAGE_PAGE (Generic Desktop)
    0x09, 0x3F,             //     USAGE (Reserved)
    0x09, 0x3B,             //     USAGE (Byte Count)
    0x81, 0x01,             //     INPUT (Cnst,Ary,Abs)        — report header
    0x75, 0x01,             //     REPORT_SIZE (1)
    0x15, 0x00,             //     LOGICAL_MINIMUM (0)
    0x25, 0x01,             //     LOGICAL_MAXIMUM (1)
    0x35, 0x00,             //     PHYSICAL_MINIMUM (0)
    0x45, 0x01,             //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04,             //     REPORT_COUNT (4)
    0x05, 0x09,             //     USAGE_PAGE (Button)
    0x19, 0x0C,             //     USAGE_MINIMUM (Button 12)
    0x29, 0x0F,             //     USAGE_MAXIMUM (Button 15)
    0x81, 0x02,             //     INPUT (Data,Var,Abs)        — d-pad
    0x75, 0x01,             //     REPORT_SIZE (1)
    0x15, 0x00,             //     LOGICAL_MINIMUM (0)
    0x25, 0x01,             //     LOGICAL_MAXIMUM (1)
    0x35, 0x00,             //     PHYSICAL_MINIMUM (0)
    0x45, 0x01,             //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04,             //     REPORT_COUNT (4)
    0x05, 0x09,             //     USAGE_PAGE (Button)
    0x09, 0x09,             //     USAGE (Button 9)            — start
    0x09, 0x0A,             //     USAGE (Button 10)           — back
    0x09, 0x07,             //     USAGE (Button 7)            — left stick
    0x09, 0x08,             //     USAGE (Button 8)            — right stick
    0x81, 0x02,             //     INPUT (Data,Var,Abs)
    0x75, 0x01,             //     REPORT_SIZE (1)
    0x15, 0x00,             //     LOGICAL_MINIMUM (0)
    0x25, 0x01,             //     LOGICAL_MAXIMUM (1)
    0x35, 0x00,             //     PHYSICAL_MINIMUM (0)
    0x45, 0x01,             //     PHYSICAL_MAXIMUM (1)
    0x95, 0x03,             //     REPORT_COUNT (3)
    0x05, 0x09,             //     USAGE_PAGE (Button)
    0x09, 0x05,             //     USAGE (Button 5)            — LB
    0x09, 0x06,             //     USAGE (Button 6)            — RB
    0x09, 0x0B,             //     USAGE (Button 11)           — guide
    0x81, 0x02,             //     INPUT (Data,Var,Abs)
    0x75, 0x01,             //     REPORT_SIZE (1)
    0x95, 0x01,             //     REPORT_COUNT (1)
    0x81, 0x01,             //     INPUT (Cnst,Ary,Abs)        — padding
    0x75, 0x01,             //     REPORT_SIZE (1)
    0x15, 0x00,             //     LOGICAL_MINIMUM (0)
    0x25, 0x01,             //     LOGICAL_MAXIMUM (1)
    0x35, 0x00,             //     PHYSICAL_MINIMUM (0)
    0x45, 0x01,             //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04,             //     REPORT_COUNT (4)
    0x05, 0x09,             //     USAGE_PAGE (Button)
    0x19, 0x01,             //     USAGE_MINIMUM (Button 1)
    0x29, 0x04,             //     USAGE_MAXIMUM (Button 4)
    0x81, 0x02,             //     INPUT (Data,Var,Abs)        — A B X Y
    0x75, 0x08,             //     REPORT_SIZE (8)
    0x15, 0x00,             //     LOGICAL_MINIMUM (0)
    0x26, 0xFF, 0x00,       //     LOGICAL_MAXIMUM (255)
    0x35, 0x00,             //     PHYSICAL_MINIMUM (0)
    0x46, 0xFF, 0x00,       //     PHYSICAL_MAXIMUM (255)
    0x95, 0x02,             //     REPORT_COUNT (2)
    0x05, 0x01,             //     USAGE_PAGE (Generic Desktop)
    0x09, 0x32,             //     USAGE (Z)                   — left trigger
    0x09, 0x35,             //     USAGE (Rz)                  — right trigger
    0x81, 0x02,             //     INPUT (Data,Var,Abs)
    0x75, 0x10,             //     REPORT_SIZE (16)
    0x16, 0x00, 0x80,       //     LOGICAL_MINIMUM (-32768)
    0x26, 0xFF, 0x7F,       //     LOGICAL_MAXIMUM (32767)
    0x36, 0x00, 0x80,       //     PHYSICAL_MINIMUM (-32768)
    0x46, 0xFF, 0x7F,       //     PHYSICAL_MAXIMUM (32767)
    0x05, 0x01,             //     USAGE_PAGE (Generic Desktop)
    0x09, 0x01,             //     USAGE (Pointer)
    0xA1, 0x00,             //     COLLECTION (Physical)
    0x95, 0x02,             //       REPORT_COUNT (2)
    0x05, 0x01,             //       USAGE_PAGE (Generic Desktop)
    0x09, 0x30,             //       USAGE (X)
    0x09, 0x31,             //       USAGE (Y)
    0x81, 0x02,             //       INPUT (Data,Var,Abs)      — left stick
    0xC0,                   //     END_COLLECTION
    0x05, 0x01,             //     USAGE_PAGE (Generic Desktop)
    0x09, 0x01,             //     USAGE (Pointer)
    0xA1, 0x00,             //     COLLECTION (Physical)
    0x95, 0x02,             //       REPORT_COUNT (2)
    0x05, 0x01,             //       USAGE_PAGE (Generic Desktop)
    0x09, 0x33,             //       USAGE (Rx)
    0x09, 0x34,             //       USAGE (Ry)
    0x81, 0x02,             //       INPUT (Data,Var,Abs)      — right stick
    0xC0,                   //     END_COLLECTION
    0xC0,                   //   END_COLLECTION
    0xC0,                   // END_COLLECTION
};
const size_t X360HIDReportDescriptorSize = sizeof(X360HIDReportDescriptor);

const uint8_t *X360HIDReportDescriptorBytes(void)
{
    return X360HIDReportDescriptor;
}

size_t X360BuildHIDReport(const X360PadState *state, uint8_t *out, size_t capacity)
{
    if (state == NULL || out == NULL || capacity < X360_HID_REPORT_SIZE) {
        return 0;
    }
    Clear(out, X360_HID_REPORT_SIZE);
    out[0] = 0x00;
    out[1] = X360_HID_REPORT_SIZE;
    WriteLE16(out + kReportButtons, state->buttons);
    out[kReportLeftTrigger]  = state->leftTrigger;
    out[kReportRightTrigger] = state->rightTrigger;
    WriteLE16(out + kReportLeftX,  (uint16_t)state->leftX);
    // Bitwise NOT (not negation) maps the full range, including -32768, like 360Controller.
    WriteLE16(out + kReportLeftY,  (uint16_t)~state->leftY);
    WriteLE16(out + kReportRightX, (uint16_t)state->rightX);
    WriteLE16(out + kReportRightY, (uint16_t)~state->rightY);
    return X360_HID_REPORT_SIZE;
}

bool X360ParseOutputReport(const uint8_t *report, size_t length, X360OutputRequest *out)
{
    if (out == NULL) {
        return false;
    }
    Clear(out, sizeof(*out));
    if (report == NULL || length < 2) {
        return false;
    }
    switch (report[0]) {
        case 0x00:
            if (length == 4 && report[1] == 0x04) {
                out->type = X360_OUTPUT_RUMBLE;
                out->large = report[2];
                out->small = report[3];
                return true;
            }
            break;
        case 0x01:
            if (length == 3 && report[1] == 0x03) {
                out->type = X360_OUTPUT_LED;
                out->pattern = report[2];
                return true;
            }
            break;
        case 0x02:
            out->type = X360_OUTPUT_POWER_OFF;
            return true;
        default:
            break;
    }
    return false;
}
