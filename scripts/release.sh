#!/bin/bash
#
# Builds, signs (Developer ID), notarizes and packages X360DriverManager.app
# (with the embedded X360Driver.dext) for a GitHub release.
#
# Requirements: see CONTRIBUTOR.md. Usage:
#
#   TEAM_ID=ABCDE12345 NOTARY_PROFILE=x360-notary VERSION=0.1.0 scripts/release.sh
#
set -euo pipefail

: "${TEAM_ID:?Set TEAM_ID to your 10-character Apple Developer Team ID}"
: "${NOTARY_PROFILE:?Set NOTARY_PROFILE to the name used with 'xcrun notarytool store-credentials'}"
: "${VERSION:?Set VERSION, e.g. 0.1.0}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/release"
ARCHIVE="$OUT/X360DriverManager.xcarchive"
EXPORT="$OUT/export"
APP="$EXPORT/X360DriverManager.app"
ZIP="$OUT/X360DriverManager-$VERSION.zip"

# xcode-select may point at the Command Line Tools; always use full Xcode.
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"

echo "==> Running unit tests"
(cd "$ROOT" && swift test)

echo "==> Archiving version $VERSION for team $TEAM_ID"
rm -rf "$OUT"
mkdir -p "$OUT"
xcodebuild \
    -project "$ROOT/Driver/Xbox360Driver.xcodeproj" \
    -scheme X360DriverManager \
    -configuration Release \
    -archivePath "$ARCHIVE" \
    -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$TEAM_ID" \
    MARKETING_VERSION="$VERSION" \
    archive

echo "==> Exporting with Developer ID signing"
cat > "$OUT/ExportOptions.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>
    <string>developer-id</string>
    <key>teamID</key>
    <string>$TEAM_ID</string>
    <key>signingStyle</key>
    <string>automatic</string>
</dict>
</plist>
EOF
xcodebuild -exportArchive \
    -archivePath "$ARCHIVE" \
    -exportPath "$EXPORT" \
    -exportOptionsPlist "$OUT/ExportOptions.plist" \
    -allowProvisioningUpdates

echo "==> Checking signatures and entitlements"
DEXT="$APP/Contents/Library/SystemExtensions/X360Driver.dext"
codesign --verify --deep --strict --verbose=2 "$APP"
for required in com.apple.developer.driverkit \
                com.apple.developer.driverkit.transport.usb \
                com.apple.developer.driverkit.family.hid.device; do
    if ! codesign -d --entitlements - "$DEXT" 2>/dev/null | grep -q "$required"; then
        echo "error: the signed dext lacks $required — has Apple granted it to team $TEAM_ID?" >&2
        exit 1
    fi
done
codesign -d --entitlements - "$APP" 2>/dev/null | grep -q com.apple.developer.system-extension.install \
    || { echo "error: the app lacks the System Extension entitlement" >&2; exit 1; }

echo "==> Notarizing (this usually takes a few minutes)"
ditto -c -k --keepParent "$APP" "$OUT/notarize.zip"
xcrun notarytool submit "$OUT/notarize.zip" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute --verbose=2 "$APP"

echo "==> Packaging"
ditto -c -k --keepParent "$APP" "$ZIP"
(cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" > "$(basename "$ZIP").sha256")

echo
echo "Done:"
echo "  $ZIP"
echo "  $ZIP.sha256"
