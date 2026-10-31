# Xbox 360 Wireless Receiver driver for macOS (DriverKit) — Plan

## Goal

Make an Xbox 360 controller that is paired to the Xbox 360 Wireless Receiver
(USB `045E:0291`, also `045E:0719`, `045E:02A9`) usable in macOS games, using a
DriverKit extension (dext) — kernel extensions like the old
[360Controller](../360Controller) are effectively dead on Apple silicon.

## Findings that shape the design

| Topic | Finding | Consequence |
| --- | --- | --- |
| Hardware | Receiver enumerates as `045E:0291`, class `FF/FF/FF`, no Apple driver claims it, `IOServiceDEXTEntitlements = transport.usb` | A USBDriverKit dext can match the `IOUSBHostDevice` directly |
| Protocol | Receiver has 4 controller slots; each slot = interface with protocol `0x81` (controller, 1 IN + 1 OUT interrupt pipe) + protocol `0x82` (headset/"mystery", ignored) | One USB service, up to 4 virtual gamepads |
| Game compatibility | GameController.framework only accepts Apple-whitelisted controllers. macOS 14+ uses the **Xbox 360 wired controller** as its HID reference; SDL already ships mappings for the 360Controller layout (`045E:028E`) | Publish each pad as an `IOUserHIDDevice` using exactly 360Controller's HID descriptor, report format, VID/PID and output reports → SDL, Steam, browsers, IOKit games just work |
| Signing | No signing identities and no Apple ID in Xcode on this Mac; SIP enabled | **Blocker for loading** (not for building). See "Decision needed" |

## Architecture

```
360ControllerMacDriver/
├── Core/                      Portable C99 protocol library (no allocation, no OS deps)
│   ├── X360Protocol.h/.c      parse receiver packets, build LED/rumble/power/inquiry
│   │                          packets, convert pad state → HID report, HID descriptor
├── Package.swift              SwiftPM: builds Core + tests + probe tool with `swift build/test`
├── Tests/X360ProtocolTests    Swift Testing unit tests for Core (packet fixtures)
├── Tools/x360probe            User-space CLI (IOUSBHost.framework): talks to the
│                              receiver directly, prints live input, sets LEDs/rumble.
│                              Needs NO signing → validates protocol on real hardware now
└── Driver/
    ├── Xbox360Driver.xcodeproj
    ├── X360Driver/            the dext (USBDriverKit + HIDDriverKit)
    │   ├── X360WirelessReceiver.iig/.cpp   IOService on IOUSBHostDevice: sets config,
    │   │                                   opens 4 slot interfaces, async reads, creates
    │   │                                   / destroys pads on connect/disconnect
    │   ├── X360WirelessGamepad.iig/.cpp    IOUserHIDDevice per connected pad: report
    │   │                                   descriptor, handleReport, setReport (rumble/LED)
    │   ├── Info.plist (personalities), X360Driver.entitlements
    └── X360DriverManager/     SwiftUI host app: activates/deactivates the dext via
                               OSSystemExtensionRequest, shows status & instructions
```

Data flow: `USB IN pipe (slot n)` → receiver parses with Core →
`connect` → `IOService::Create` an `X360WirelessGamepad` for slot n, set LED to player n+1 →
`input` → gamepad converts to 20-byte 360 HID report → `handleReport` →
IOHIDFamily → games. Reverse path: game `setReport` (rumble `00 04 big small`,
LED `01 03 n`) → gamepad → receiver writes to OUT pipe.

## Status (2026-10-31)

| Phase | State |
| --- | --- |
| 1 Core + tests | ✅ 17 Swift Testing tests pass (`swift test`) |
| 2 x360probe | ✅ Verified on hardware: 4 slots (EP 0x81/83/85/87 IN, 0x01/03/05/07 OUT), connect, announcement (serial, battery), battery updates, inquiry, writes. ⏳ Stick/button decoding needs a human pressing buttons |
| 3 Dext | ✅ Builds for DriverKit 27.2 (arm64 + x86_64), no warnings. ⏳ Not loaded yet (signing) |
| 4 Host app | ✅ Builds, embeds the dext in `Contents/Library/SystemExtensions` |
| 5 Load & test | ⛔ Blocked on the signing decision below — follow SETUP.md |

Learned on hardware: interrupt OUT transfers must use completion timeout 0
(a 1 s timeout fails with `kIOReturnBadArgument`).

## Phases

1. **Core protocol + tests** — packet parser/builders, ported from 360Controller
   and cross-checked against Linux `xpad`. Fully testable without hardware.
2. **x360probe** — talk to the real receiver from user space to confirm reads,
   connect/disconnect, battery, LED, rumble. (Works without any signing.)
3. **Dext** — receiver service + HID gamepad service, builds against DriverKit SDK.
4. **Host app** — activation UI; embeds dext in `Contents/Library/SystemExtensions`.
5. **Load & test on hardware** — needs the signing decision below. Verify with
   `ioreg`, `log stream --predicate 'sender CONTAINS "X360Driver"'`, a HID tester,
   SDL/Steam/a browser gamepad tester.
6. **Polish (later weekends)** — settings (deadzones, inversion) via user client,
   battery as HID property, auto power-off, wired 360 controller (`045E:028E`)
   support (same report format, simpler), installer/notarization notes.

## Decision needed from you (loading the dext)

Building works without signing; **activating** a dext requires one of:

- **A. Paid Apple Developer Program team** signed into Xcode → Settings → Accounts.
  Xcode's automatic signing provides development-only DriverKit entitlements
  (`driverkit`, `driverkit.transport.usb`, `driverkit.family.hid.device`).
  App must be run from `/Applications` (or enable `systemextensionsctl developer on`).
  Distribution to other Macs would later need Apple to grant the entitlements.
- **B. No paid account** → disable SIP (Recovery → `csrutil disable`), then
  `systemextensionsctl developer on`, ad-hoc sign. Lowers your Mac's security
  while enabled; re-enable when done. A free "Personal Team" can't sign dexts.

## Risks / open questions

- Brave and Logitech G HUB currently hold user clients on the receiver; harmless
  for reading, but if one of them opens the device exclusively the dext's `Open`
  would fail. Verify during phase 5.
- Third-party "0291" receivers sometimes need a re-plug after configuration.
- GameController.framework (Apple Arcade-style games) will *not* see the pad;
  only IOKit-HID-based games (SDL, Unity's old input, Steam, browsers, emulators).
