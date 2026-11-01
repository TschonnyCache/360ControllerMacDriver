# Signing a release — guide for Apple Developer Program members

Thank you for helping! This project is a macOS driver for the Xbox 360 Wireless
Receiver. The code builds fine, but macOS only loads a driver (a *DriverKit
extension*) on other people's Macs if it is

1. signed with a **Developer ID** certificate,
2. carrying **DriverKit entitlements that Apple has granted to the signing team**, and
3. **notarized** by Apple.

All three need a paid Apple Developer Program membership, which the maintainer
doesn't have. This document explains what you, as a member, would do.

## What it costs you

- **Time:** about 30 minutes of setup, plus waiting for Apple to approve an
  entitlement request (days to weeks). After that, about 10 minutes per release.
- **Money:** nothing beyond your existing membership.
- **Visibility:** the app is signed with *your* team. Your team name appears in
  the macOS approval prompt and in `systemextensionsctl list`. Users will see
  that you vouched for this build, so only sign code you have looked at.
- **Ongoing:** every new release must be signed by your team again (or by
  whoever takes over).

## Who on the team must do what

| Task | Required role |
| --- | --- |
| Request the DriverKit entitlements | Account Holder |
| Create the Developer ID Application certificate | Account Holder (or an Admin with Developer ID access) |
| Register App IDs, build, notarize | Admin or App Manager is enough |

---

## Part 1 — One-time setup (human steps in the browser)

### 1.1 Request the DriverKit entitlements

Apple reviews these by hand.

1. Open <https://developer.apple.com/contact/request/system-extension/> and sign in.
2. Choose **DriverKit** and request this group of entitlements for one product:
   - `com.apple.developer.driverkit`
   - `com.apple.developer.driverkit.transport.usb`, with **USB Vendor ID `0x045E`** (1118, Microsoft)
   - `com.apple.developer.driverkit.family.hid.device`
3. Suggested description:
   > Open-source driver (github.com/TschonnyCache/360ControllerMacDriver) for the
   > discontinued Xbox 360 Wireless Gaming Receiver (USB 045E:0719 / 0291 / 02A9).
   > Apple's own drivers don't support this receiver, and the previous kernel
   > extension (360Controller) no longer loads on Apple silicon. The dext matches
   > the receiver's IOUSBHostDevice and publishes one HID game pad per connected
   > controller. It is delivered in a host app via SystemExtensions.framework.
4. Wait for Apple's email. Apple then attaches the approved entitlements to your
   team as a named "group".

> **Be aware:** Apple may push back because neither of us owns the Microsoft
> vendor ID. If they ask for more information, point to the open-source
> repository and the fact that the hardware is discontinued. If they refuse,
> please tell the maintainer.

### 1.2 Create a Developer ID Application certificate

You can skip this if you already have one.

- **In Xcode:** Settings ▸ Accounts ▸ select your team ▸ Manage Certificates ▸
  **+** ▸ *Developer ID Application*.
- **Or on the website:** <https://developer.apple.com/account/resources/certificates>.

### 1.3 Register the two App IDs

Do this at <https://developer.apple.com/account/resources/identifiers>, using **+** ▸ *App IDs* ▸ *App*.

| Description | Bundle ID (explicit) | Capabilities to enable |
| --- | --- | --- |
| X360 Driver Manager | `com.jonaskreusch.X360DriverManager` | **System Extension** |
| X360 Driver (dext) | `com.jonaskreusch.X360DriverManager.X360Driver` | (none here; see below) |

For the dext App ID, open the **Additional Capabilities** tab and select the
DriverKit entitlement group Apple granted you in 1.1.

If either bundle ID is already taken, pick your own prefix and change it in these
places, then commit:

- `Driver/Xbox360Driver.xcodeproj` (`PRODUCT_BUNDLE_IDENTIFIER` of both the app and the dext target)
- `Driver/X360DriverManager/DriverActivator.swift` (`driverIdentifier`)

The dext's ID must stay `<app ID>.X360Driver`.

### 1.4 Create an app-specific password for notarization

1. Open <https://account.apple.com> ▸ Sign-In and Security ▸ App-Specific Passwords ▸ **+**.
2. Name it "x360 notarization" and copy the password.

---

## Part 2 — One-time setup (commands)

```sh
# Clone the project
git clone https://github.com/TschonnyCache/360ControllerMacDriver.git
cd 360ControllerMacDriver

# Make sure the full Xcode is used (not just the Command Line Tools)
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer

# Find your Team ID (10 characters, e.g. ABCDE12345)
security find-identity -v -p codesigning | grep "Developer ID Application"
#   -> "Developer ID Application: Your Name (ABCDE12345)"

# Store the notarization credentials in your keychain under the name "x360-notary"
xcrun notarytool store-credentials x360-notary \
    --apple-id "you@example.com" \
    --team-id ABCDE12345 \
    --password "the-app-specific-password-from-1.4"
```

Also sign in to Xcode once (Settings ▸ Accounts) with the same Apple ID, so the
command-line build can create the provisioning profiles automatically.

---

## Part 3 — Making a release (every version)

```sh
cd 360ControllerMacDriver
git pull
git checkout v0.1.0        # the tag/commit the maintainer asked you to sign

TEAM_ID=ABCDE12345 NOTARY_PROFILE=x360-notary VERSION=0.1.0 scripts/release.sh
```

The script:

1. runs the unit tests;
2. archives the app and dext;
3. exports them with Developer ID signing;
4. **stops with an error if the dext lacks the DriverKit entitlements** (meaning 1.1 or 1.3 isn't complete);
5. notarizes and staples;
6. writes two files to `build/release/`:
   - `X360DriverManager-0.1.0.zip`, the file users download;
   - `X360DriverManager-0.1.0.zip.sha256`, its checksum.

### Hand the build over

Choose one:

- **If you have write access to the repository:**
  ```sh
  gh release create v0.1.0 \
      build/release/X360DriverManager-0.1.0.zip \
      build/release/X360DriverManager-0.1.0.zip.sha256 \
      --title "v0.1.0" --notes "Signed and notarized by <your name/team>."
  ```
- **Otherwise:** send both files to the maintainer, who uploads them to the GitHub release.

### Optional: test before handing over

1. Unzip the archive and move `X360DriverManager.app` to `/Applications`.
2. Open it and click **Activate Driver**.
3. Approve in System Settings ▸ General ▸ Login Items & Extensions ▸ Driver Extensions.
4. Replug the receiver, then check:

```sh
systemextensionsctl list          # ...X360Driver... [activated enabled]
log stream --predicate 'eventMessage CONTAINS "X360Driver"'
```

You don't need an Xbox receiver to sign a release. Activation alone already
proves that the signature and entitlements are accepted.

---

## Troubleshooting

| Message | Meaning |
| --- | --- |
| `the signed dext lacks com.apple.developer.driverkit...` | Apple hasn't granted the entitlement yet, or the group isn't attached to the dext App ID (1.3). |
| `No profiles for 'com.jonaskreusch...' were found` | The App ID isn't registered under your team, or Xcode isn't signed in (Part 2). |
| `No signing certificate "Developer ID Application" found` | Do 1.2. |
| Notarization `Invalid`, then run `xcrun notarytool log <id> --keychain-profile x360-notary` | Send the log to the maintainer. |
| `Provisioning profile ... doesn't include the com.apple.developer.driverkit.family.hid.device entitlement` | Ask Apple to add that entitlement to your DriverKit group. |
