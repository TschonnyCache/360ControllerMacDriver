//
//  ContentView.swift
//  X360DriverManager
//

import SwiftUI

struct ContentView: View {
    @State private var activator = DriverActivator()
    @State private var monitor = DeviceMonitor()

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
                }
                .disabled(activator.state.isWorking)
            }

            Section("Hardware") {
                LabeledContent("Wireless receiver") {
                    StatusText(isOn: monitor.receiverConnected, on: "Connected", off: "Not found")
                }
                LabeledContent("Driver attached") {
                    StatusText(isOn: monitor.driverAttached, on: "Yes", off: "No")
                }
            }

            Section("Controllers") {
                if monitor.gamepads.isEmpty {
                    Text("No controllers. Press the guide button on a paired controller.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(monitor.gamepads) { pad in
                        LabeledContent(pad.name) {
                            if let level = pad.batteryLevel {
                                Label("\(level * 100 / 255) %", systemImage: "battery.75percent")
                            } else {
                                Text(pad.serial).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 420, minHeight: 360)
        .task { await monitor.run() }
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
