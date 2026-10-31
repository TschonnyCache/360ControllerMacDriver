//
//  X360Protocol.h
//  Portable (C99, no allocation, no OS dependencies) implementation of the
//  Xbox 360 Wireless Receiver protocol. Shared by the DriverKit extension,
//  the user-space probe tool and the unit tests.
//
//  Protocol knowledge comes from the 360Controller project (Colin Munro et al.)
//  and the Linux xpad driver.
//

#ifndef X360PROTOCOL_H
#define X360PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MARK: - USB identity

#define X360_VENDOR_MICROSOFT              0x045E
#define X360_PRODUCT_WIRELESS_RECEIVER     0x0719  // Official "for Windows" receiver
#define X360_PRODUCT_WIRELESS_RECEIVER_2   0x0291  // Common (often third-party) receiver
#define X360_PRODUCT_WIRELESS_RECEIVER_3   0x02A9  // Another third-party variant
#define X360_PRODUCT_WIRED_CONTROLLER      0x028E  // Identity used for the virtual pads

/// Number of controller slots a receiver exposes.
#define X360_MAX_SLOTS                     4

/// bInterfaceProtocol of a slot's controller interface (one IN + one OUT interrupt pipe).
#define X360_INTERFACE_PROTOCOL_CONTROLLER 0x81
/// bInterfaceProtocol of a slot's headset interface (unused).
#define X360_INTERFACE_PROTOCOL_HEADSET    0x82

/// Largest packet the receiver sends on a slot's IN pipe.
#define X360_WIRELESS_MAX_PACKET           32
/// Size of every command sent to a slot's OUT pipe.
#define X360_WIRELESS_COMMAND_SIZE         12

// MARK: - Pad state

/// Button bit positions, matching the little-endian 16-bit word at bytes 2–3 of
/// an Xbox 360 input report.
enum {
    X360_BUTTON_DPAD_UP    = 1u << 0,
    X360_BUTTON_DPAD_DOWN  = 1u << 1,
    X360_BUTTON_DPAD_LEFT  = 1u << 2,
    X360_BUTTON_DPAD_RIGHT = 1u << 3,
    X360_BUTTON_START      = 1u << 4,
    X360_BUTTON_BACK       = 1u << 5,
    X360_BUTTON_LSTICK     = 1u << 6,
    X360_BUTTON_RSTICK     = 1u << 7,
    X360_BUTTON_LB         = 1u << 8,
    X360_BUTTON_RB         = 1u << 9,
    X360_BUTTON_GUIDE      = 1u << 10,
    X360_BUTTON_A          = 1u << 12,
    X360_BUTTON_B          = 1u << 13,
    X360_BUTTON_X          = 1u << 14,
    X360_BUTTON_Y          = 1u << 15,
};

/// Decoded controller state. Stick values use the controller's native
/// convention: Y is positive when the stick is pushed *up*.
typedef struct X360PadState {
    uint16_t buttons;
    uint8_t  leftTrigger;
    uint8_t  rightTrigger;
    int16_t  leftX;
    int16_t  leftY;
    int16_t  rightX;
    int16_t  rightY;
} X360PadState;

// MARK: - Receiver → host

typedef enum X360EventType {
    X360_EVENT_NONE = 0,       ///< Packet carried nothing we use.
    X360_EVENT_CONNECTED,      ///< A controller is present in this slot.
    X360_EVENT_DISCONNECTED,   ///< The slot is now empty.
    X360_EVENT_INPUT,          ///< `state` holds new input.
    X360_EVENT_INFO,           ///< Controller announcement: `serial` valid, `battery` valid if `hasBattery`.
    X360_EVENT_BATTERY,        ///< `battery` holds a new level.
} X360EventType;

typedef struct X360Event {
    X360EventType type;
    X360PadState  state;
    /// Battery level as reported by the controller (0x00–0xFF; 0xFF is full).
    uint8_t       battery;
    bool          hasBattery;
    /// True when a headset is attached (only meaningful for CONNECTED).
    bool          headset;
    uint8_t       serial[4];
} X360Event;

/// Decodes one packet read from a slot's controller IN pipe.
/// Returns false (and sets `out->type` to X360_EVENT_NONE) for packets that are
/// malformed or carry nothing of interest.
bool X360ParseWirelessPacket(const uint8_t *data, size_t length, X360Event *out);

/// Decodes the common 360 input report layout (`00 len buttons… `) found both
/// inside wireless packets and in wired controller reports.
bool X360ParseInputReport(const uint8_t *report, size_t length, X360PadState *out);

// MARK: - Host → receiver (all produce X360_WIRELESS_COMMAND_SIZE bytes)

/// LED animations understood by the controller ring.
typedef enum X360LEDPattern {
    X360_LED_OFF              = 0,
    X360_LED_BLINK_ALL        = 1,
    X360_LED_FLASH_THEN_ON_1  = 2,
    X360_LED_FLASH_THEN_ON_2  = 3,
    X360_LED_FLASH_THEN_ON_3  = 4,
    X360_LED_FLASH_THEN_ON_4  = 5,
    X360_LED_ON_1             = 6,
    X360_LED_ON_2             = 7,
    X360_LED_ON_3             = 8,
    X360_LED_ON_4             = 9,
    X360_LED_ROTATE           = 10,
    X360_LED_BLINK            = 11,
    X360_LED_SLOW_BLINK       = 12,
    X360_LED_ALTERNATE        = 13,
} X360LEDPattern;

/// LED pattern a slot should show while its controller is active.
X360LEDPattern X360LEDPatternForSlot(unsigned slot);

/// Writes a "set LED ring" command. Returns the number of bytes written (or 0 if `capacity` is too small).
size_t X360BuildLEDCommand(uint8_t pattern, uint8_t *out, size_t capacity);
/// Writes a "set rumble motors" command (`large` = left/low-frequency motor).
size_t X360BuildRumbleCommand(uint8_t large, uint8_t small, uint8_t *out, size_t capacity);
/// Writes a "turn controller off" command.
size_t X360BuildPowerOffCommand(uint8_t *out, size_t capacity);
/// Writes the command 360Controller sends after connect / LED changes to
/// (re)enable input reporting.
size_t X360BuildEnableReportingCommand(uint8_t *out, size_t capacity);
/// Writes a command asking the receiver to report whether a controller is
/// present in the slot (answered with a connection status packet).
size_t X360BuildInquiryCommand(uint8_t *out, size_t capacity);

// MARK: - Virtual HID gamepad

/// Size of the HID input report produced by `X360BuildHIDReport`
/// (identical to a wired Xbox 360 controller report).
#define X360_HID_REPORT_SIZE 20

/// The HID report descriptor used by the 360Controller driver. Using the same
/// descriptor, VID/PID and report format keeps existing SDL/Steam mappings working.
extern const uint8_t X360HIDReportDescriptor[];
extern const size_t  X360HIDReportDescriptorSize;
/// Same bytes as `X360HIDReportDescriptor`, for languages that can't import incomplete arrays (Swift).
const uint8_t *X360HIDReportDescriptorBytes(void);

/// Serialises `state` into a 20-byte wired-format report. Y axes are inverted
/// (positive = down) exactly like 360Controller does by default, which is what
/// HID consumers and existing game mappings expect.
size_t X360BuildHIDReport(const X360PadState *state, uint8_t *out, size_t capacity);

// MARK: - HID output reports (set by games/tools, same format as 360Controller)

typedef enum X360OutputRequestType {
    X360_OUTPUT_UNSUPPORTED = 0,
    X360_OUTPUT_RUMBLE,        ///< `00 04 large small`
    X360_OUTPUT_LED,           ///< `01 03 pattern`
    X360_OUTPUT_POWER_OFF,     ///< `02 …`
} X360OutputRequestType;

typedef struct X360OutputRequest {
    X360OutputRequestType type;
    uint8_t large;
    uint8_t small;
    uint8_t pattern;
} X360OutputRequest;

/// Decodes a HID output report written to a virtual pad.
bool X360ParseOutputReport(const uint8_t *report, size_t length, X360OutputRequest *out);

#ifdef __cplusplus
}
#endif

#endif // X360PROTOCOL_H
