//
//  DriverActivator.swift
//  X360DriverManager
//
//  Activates / deactivates the embedded X360Driver dext through the
//  SystemExtensions framework and reports what happened.
//

import Foundation
import Observation
import SystemExtensions

@MainActor
@Observable
final class DriverActivator: NSObject {
    enum State: Equatable {
        case idle
        case working(String)
        case needsApproval
        case activated
        case deactivated
        case failed(String)
    }

    /// Bundle identifier of the embedded dext (must match the dext target's PRODUCT_BUNDLE_IDENTIFIER).
    static let driverIdentifier = "com.jonaskreusch.X360DriverManager.X360Driver"

    private(set) var state: State = .idle
    /// Whether the request in flight is a deactivation (decides the final state).
    private var isDeactivating = false

    func activate() {
        isDeactivating = false
        submit(.activationRequest(forExtensionWithIdentifier: Self.driverIdentifier, queue: .main),
               message: "Activating driver…")
    }

    func deactivate() {
        isDeactivating = true
        submit(.deactivationRequest(forExtensionWithIdentifier: Self.driverIdentifier, queue: .main),
               message: "Deactivating driver…")
    }

    private func submit(_ request: OSSystemExtensionRequest, message: String) {
        state = .working(message)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
}

extension DriverActivator: OSSystemExtensionRequestDelegate {
    nonisolated func request(_ request: OSSystemExtensionRequest,
                             actionForReplacingExtension existing: OSSystemExtensionProperties,
                             withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        // Always let a freshly built driver replace the installed one during development.
        .replace
    }

    nonisolated func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        MainActor.assumeIsolated { state = .needsApproval }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFinishWithResult result: OSSystemExtensionRequest.Result) {
        MainActor.assumeIsolated {
            switch result {
            case .completed:
                state = isDeactivating ? .deactivated : .activated
            case .willCompleteAfterReboot:
                state = .failed("The change takes effect after a restart.")
            @unknown default:
                state = .failed("Unknown result \(result.rawValue)")
            }
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let message = Self.describe(error)
        MainActor.assumeIsolated { state = .failed(message) }
    }

    /// Turns the common SystemExtensions errors into actionable messages.
    nonisolated private static func describe(_ error: Error) -> String {
        let nsError = error as NSError
        guard nsError.domain == OSSystemExtensionErrorDomain,
              let code = OSSystemExtensionError.Code(rawValue: nsError.code) else {
            return error.localizedDescription
        }
        switch code {
        case .unsupportedParentBundleLocation:
            return "Move X360DriverManager to /Applications (or run `systemextensionsctl developer on`)."
        case .codeSignatureInvalid, .validationFailed:
            return "The driver's signature or entitlements were rejected. See SETUP.md for signing options."
        case .extensionNotFound:
            return "The driver isn't embedded in this app. Rebuild the X360DriverManager scheme."
        case .requestCanceled:
            return "The request was cancelled."
        default:
            return "\(error.localizedDescription) (code \(nsError.code))"
        }
    }
}
