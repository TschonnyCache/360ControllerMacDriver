// swift-tools-version: 6.0
//
// Builds the portable protocol core, its unit tests and the user-space probe
// tool. The DriverKit extension itself lives in Driver/Xbox360Driver.xcodeproj
// and compiles the same Core/X360Protocol.c.

import PackageDescription

let package = Package(
    name: "Xbox360Driver",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "X360Protocol", targets: ["X360Protocol"]),
        .executable(name: "x360probe", targets: ["x360probe"]),
    ],
    targets: [
        .target(
            name: "X360Protocol",
            path: "Core"
        ),
        .executableTarget(
            name: "x360probe",
            dependencies: ["X360Protocol"],
            path: "Tools/x360probe",
            linkerSettings: [.linkedFramework("IOUSBHost"), .linkedFramework("IOKit")]
        ),
        .testTarget(
            name: "X360ProtocolTests",
            dependencies: ["X360Protocol"],
            path: "Tests/X360ProtocolTests"
        ),
    ]
)
