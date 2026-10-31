# Setup & testing guide

## 0. Quick protocol check (no signing, works today)

```sh
cd ~/Code/360ControllerMacDriver
swift test                              # 17 protocol unit tests
swift run x360probe --seconds 30        # press buttons / move sticks / pull triggers
swift run x360probe --rumble            # short rumble when the pad is detected
swift run x360probe --raw               # also dump every raw packet
```

Expected: `[slot 0] controller connected`, an announcement with serial + battery,
then one line per input change, e.g.
`[slot 0] L(  -120,   340) R(   0,    0) LT   0 RT 255 A RB`.
The ring on the controller should switch to "player 1".
Things to check by eye: **stick Y positive when pushed up**, every button name
correct, triggers 0–255. If anything is off, run with `--raw` and keep the output.

> x360probe takes exclusive ownership of the receiver while it runs. Quit it
> before testing the driver.

## 1. Pick a signing route (needed to *load* the dext)

### Route A — paid Apple Developer team (recommended, SIP stays on)

1. Xcode ▸ Settings ▸ Accounts ▸ add your Apple ID (team must be in the paid program;
   free "Personal Teams" can't sign DriverKit extensions or the System Extension capability).
2. Open `Driver/Xbox360Driver.xcodeproj`, select each target ▸ Signing & Capabilities ▸
   choose your Team. If the bundle ID `com.jonaskreusch.X360DriverManager` is taken,
   change the prefix in **both** targets *and* `DriverActivator.driverIdentifier`.
3. If Xcode reports that the profile lacks `com.apple.developer.driverkit.transport.usb`
   or `…family.hid.device`, request them at
   <https://developer.apple.com/contact/request/system-extension/> (USB, vendor ID 0x045E)
   and use Route B meanwhile.
4. Build the `X360DriverManager` scheme, then copy the built app to `/Applications`
   (Product ▸ Show Build Folder) — activation is refused anywhere else.

### Route B — no paid account (lowers system security while enabled)

1. Shut down, hold the power button ▸ Options ▸ Utilities ▸ Terminal: `csrutil disable`, reboot.
2. `systemextensionsctl developer on` (also lifts the /Applications requirement).
3. In both targets set Signing Certificate to "Sign to Run Locally", build, run.
4. When finished testing: `systemextensionsctl developer off`, and `csrutil enable` from Recovery.

## 2. Activate

Run X360DriverManager ▸ **Activate Driver** ▸ approve in System Settings ▸ General ▸
Login Items & Extensions ▸ Driver Extensions. Then unplug and replug the receiver once.

## 3. Verify

```sh
systemextensionsctl list                                   # X360Driver [activated enabled]
ioreg -w0 -l | grep -E 'X360Wireless(Receiver|Gamepad)'     # receiver + one gamepad per pad
log stream --predicate 'eventMessage CONTAINS "X360Driver"' # driver log lines
```

The app shows receiver / driver / controller status. To test input use
<https://hardwaretester.com/gamepad>, Steam's controller settings, or SDL's `testcontroller`.
Rumble: most SDL games, or the Gamepad tester's vibration button.

## 4. Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| Log: `device Open failed` | Another process holds the receiver exclusively (x360probe, Brave, Logitech G HUB). Quit it, replug. |
| Log: `creating gamepad for slot N failed` | HID entitlement mismatch. Try adding `com.apple.developer.driverkit.family.hid.virtual.device` to `X360Driver.entitlements`. |
| Activation error about location | App isn't in `/Applications` (Route A) or developer mode is off (Route B). |
| Activation error about signature/entitlements | Profile lacks the DriverKit entitlements → see Route A step 3 or use Route B. |
| Pad connects but no input | Send the `--raw` probe output; some clone receivers use slightly different packets. |
