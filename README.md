# Xbox 360 Wireless Receiver driver for macOS

Use your **Xbox 360 controllers** on a modern Mac, including Apple silicon: up to
four wireless controllers through the **Xbox 360 Wireless Gaming Receiver**, and
wired Microsoft Xbox 360 controllers.

This is a ground-up rewrite of the classic
[360Controller](https://github.com/360Controller/360Controller) driver as a
**DriverKit extension**. 360Controller was a kernel extension, and macOS no longer
loads it on current systems. This driver runs in user space and is installed and
removed with a normal app, without reboots or kernel extensions.

> **Status: early development, no downloadable release yet.**
> The protocol has been verified on real hardware with the included `x360probe`
> tool, and the driver builds. Publishing a release that runs on other Macs needs
> an Apple Developer Program member to sign it. If you have a membership and
> want to help, see [CONTRIBUTOR.md](CONTRIBUTOR.md).

## Features

- Xbox 360 Wireless Gaming Receiver for Windows: official (`045E:0719`) and common
  third-party models (`045E:0291`, `045E:02A9`)
- Up to 4 wireless controllers at once, each shown as its own game pad
- Wired Microsoft Xbox 360 controllers (`045E:028E`, `045E:028F`)
- Controller ring light shows the player number (1–4)
- Rumble
- Battery level, shown in the companion app
- Wireless controllers switch off after 15 minutes without input, to save batteries
- Built-in test panel: live sticks, triggers and buttons, plus Rumble and Identify buttons
- Controllers appear exactly like they did with the old 360Controller driver, so
  existing game mappings keep working

### Which games work?

The driver presents each controller as a standard HID game pad. That works with
anything that reads game pads through IOKit/HID:

- ✅ **Steam** and most Steam games
- ✅ Games built with **SDL**, which covers most ports and indies
- ✅ Emulators (RetroArch, Dolphin, OpenEmu, …)
- ✅ Browsers (Gamepad API: cloud gaming, web games)
- ❌ Games that *only* use Apple's GameController framework (many Apple Arcade
  titles). Apple limits that framework to controllers it officially supports.

## Requirements

- macOS 14 Sonoma or later, on Apple silicon or Intel
- For wireless controllers: an Xbox 360 Wireless Gaming Receiver plugged in by USB
- Wired controllers: only Microsoft's own. Third-party wired pads (Mad Catz, PDP, …)
  would each need a separate permission from Apple.

## Installation

1. Download `X360DriverManager-x.y.z.zip` from the
   [**Releases**](https://github.com/TschonnyCache/360ControllerMacDriver/releases) page.
2. Unzip it and move **X360DriverManager** into your **Applications** folder.
   The driver can't be activated from any other location.
3. Open **X360DriverManager** and click **Activate Driver**.
4. macOS asks you to allow the driver. Open **System Settings ▸ General ▸
   Login Items & Extensions ▸ Driver Extensions** and switch on **X360Driver**.
5. Unplug the receiver and plug it back in.
6. Turn on a controller with the guide button. If it isn't paired yet, press the
   button on the receiver, then the small connect button on top of the controller.
   The ring should light up the player-1 segment. Wired controllers work as soon as
   they're plugged in.

The app shows whether the receiver is detected and which controllers are connected.
Move the sticks and press buttons to check that everything arrives. Use **Identify**
to find out which physical controller is which. You can quit the app afterwards; the
driver keeps running.

## Uninstalling

Open **X360DriverManager**, click **Deactivate**, then move the app to the Trash.
Deleting the app from Applications in the Finder also removes the driver.

## Troubleshooting

| Problem | Try |
| --- | --- |
| The app says "Move X360DriverManager to /Applications" | Move the app into the Applications folder and open it from there. |
| Activation stays at "Approve in System Settings" | Do step 4 of the installation. |
| The receiver is found but "Driver attached: No" | Unplug and replug the receiver. Quit apps that talk to USB devices directly (for example Logitech G HUB), then replug again. |
| A controller connects but doesn't work in a game | Check it in a game pad tester such as <https://hardwaretester.com/gamepad>. If it works there, the game probably only supports Apple's GameController framework (see above). |
| The ring keeps flashing all four lights | The controller isn't paired. Press the receiver's button, then the controller's connect button. |

Still stuck? [Open an issue](https://github.com/TschonnyCache/360ControllerMacDriver/issues)
and include your macOS version, the receiver's ID (System Information ▸ USB), and
the output of:

```sh
log show --last 5m --predicate 'eventMessage CONTAINS "X360Driver"'
```

## Building from source

You need Xcode 16 or later.

```sh
git clone https://github.com/TschonnyCache/360ControllerMacDriver.git
cd 360ControllerMacDriver

swift test                              # protocol unit tests
swift run x360probe --seconds 30        # talk to the receiver / a wired pad directly, no driver needed
swift run x360probe --help              # LED, rumble and power-off tests
open Driver/Xbox360Driver.xcodeproj     # driver + companion app
```

`x360probe` reads the receiver (or a wired controller) from user space and prints
controller input. It's the quickest way to check whether your hardware works at all.
It needs exclusive access, so it can't run while the driver is active.

Loading a self-built driver requires either Apple-granted DriverKit entitlements
or temporarily disabling System Integrity Protection. [SETUP.md](SETUP.md)
describes both ways, and [CONTRIBUTOR.md](CONTRIBUTOR.md) describes how releases
are signed.

### Project layout

| Path | Contents |
| --- | --- |
| `Core/` | Portable C implementation of the receiver protocol, shared by everything else |
| `Tests/` | Unit tests for the protocol ([Swift Testing](https://developer.apple.com/documentation/testing)) |
| `Tools/x360probe/` | Command-line tool that talks to the receiver directly |
| `Driver/X360Driver/` | The DriverKit extension: receiver and wired-controller services, plus one HID game pad per controller |
| `Driver/X360DriverManager/` | SwiftUI app that installs the driver and shows its status |
| `scripts/release.sh` | Signs, notarizes and packages a release |
| `PLAN.md` | Design notes and roadmap |

### How it works

```
Receiver (USB) ─▶ X360WirelessReceiver ─▶ X360Gamepad ×4 ─▶ HID ─▶ games
                  4 slots, connect/          Xbox 360 HID report,
                  disconnect, LED, rumble,   rumble/LED requests ◀── games
                  idle power-off
Wired pad (USB) ─▶ X360WiredController ──▶ X360Gamepad
```

## Contributing

Bug reports, tests with other receivers, and pull requests are welcome. Help we
especially need right now:

- **Signing releases:** you need an Apple Developer Program membership. See
  [CONTRIBUTOR.md](CONTRIBUTOR.md).
- Testing with third-party receivers, wired controllers and games

## Credits

- [360Controller](https://github.com/360Controller/360Controller) by Colin Munro
  and contributors. Most of the protocol knowledge, and the HID report
  descriptor, come from there.
- The Linux [xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)
  driver, used to cross-check the wireless protocol.

## License

Copyright © 2026 Jonas Kreusch and contributors.

This program is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License, version 2 or (at your option) any later
version** (SPDX: `GPL-2.0-or-later`), the same terms as 360Controller, which it
builds on. See [LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

*Xbox and Xbox 360 are trademarks of Microsoft. This project is not affiliated
with or endorsed by Microsoft or Apple.*
