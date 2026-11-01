//
//  ContentView.swift
//  X360DriverManager
//

import SwiftUI

struct ContentView: View {
    @State private var activator = DriverActivator()
    @State private var hardware = DeviceMonitor()
    @State private var gamepads = GamepadMonitor()

    var body: some View {
        Form {
            Section("Driver") {
                LabeledContent("Status") {
                    DriverStateLabel(state: activator.state)
                }
                HStack {
                    Button("Activate Driver", action: activator.activate)
                        .buttonStyle(.borderedProminent)
                    Button("Deactivate", action: activator.deactivate)
                    if activator.state == .needsApproval {
                        Button("Open System Settings", action: openDriverExtensionSettings)
                    }
                }
                .disabled(activator.state.isWorking)
            }

            Section("Hardware") {
                LabeledContent("Wireless receiver") {
                    StatusText(isOn: hardware.receiverConnected, on: "Connected", off: "Not found")
                }
                if hardware.wiredControllerCount > 0 {
                    LabeledContent("Wired controllers") {
                        Text("\(hardware.wiredControllerCount)")
                    }
                }
                LabeledContent("Driver attached") {
                    StatusText(isOn: hardware.driverAttached, on: "Yes", off: "No")
                }
            }

            Section("Controllers") {
                if gamepads.gamepads.isEmpty {
                    Text("No controllers. Turn on a paired controller with its guide button, or plug in a wired one.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(gamepads.gamepads) { pad in
                        ControllerTestView(pad: pad)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 480, minHeight: 420)
        .task { await hardware.run() }
        .task { await gamepads.run() }
    }

    private func openDriverExtensionSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }
}

private struct DriverStateLabel: View {
    let state: DriverActivator.State

    var body: some View {
        switch state {
        case .idle:
            Text("Not requested yet").foregroundStyle(.secondary)
        case .working(let message):
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text(message)
            }
        case .needsApproval:
            Text("Approve in System Settings → General → Login Items & Extensions → Driver Extensions")
                .foregroundStyle(.orange)
                .multilineTextAlignment(.trailing)
        case .activated:
            Text("Activated").foregroundStyle(.green)
        case .deactivated:
            Text("Deactivated").foregroundStyle(.secondary)
        case .failed(let message):
            Text(message)
                .foregroundStyle(.red)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
    }
}

private struct StatusText: View {
    let isOn: Bool
    let on: String
    let off: String

    var body: some View {
        Text(isOn ? on : off).foregroundStyle(isOn ? .green : .secondary)
    }
}

private extension DriverActivator.State {
    var isWorking: Bool {
        if case .working = self { return true }
        return false
    }
}

#Preview {
    ContentView()
}
