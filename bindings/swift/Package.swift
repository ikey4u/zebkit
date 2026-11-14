// swift-tools-version:5.9
// zebkit for iOS (docs/VIEWER.md): Zebkit.xcframework, libzebkit without the
// engine (scripts/package-mobile.sh slim-ios on a Mac), and ZebkitViewer,
// a WKWebView whose pages have zebkit's global object.
import PackageDescription

let package = Package(
    name: "Zebkit",
    platforms: [.iOS(.v15)],
    products: [
        .library(name: "ZebkitViewer", targets: ["ZebkitViewer"]),
    ],
    targets: [
        // Put here by scripts/package-mobile.sh slim-ios (xcodebuild
        // -create-xcframework), or set ZEBKIT_XCFRAMEWORK in an app's copy.
        .binaryTarget(name: "Zebkit", path: "Zebkit.xcframework"),
        .target(
            name: "ZebkitViewer",
            dependencies: ["Zebkit"],
            path: "Sources/ZebkitViewer"
        ),
        // On a simulator or device: xcodebuild test -scheme Zebkit
        // -destination 'platform=iOS Simulator,name=...'.
        .testTarget(
            name: "ZebkitViewerTests",
            dependencies: ["ZebkitViewer"],
            path: "Tests/ZebkitViewerTests"
        ),
    ]
)
