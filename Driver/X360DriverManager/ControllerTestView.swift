//
//  ControllerTestView.swift
//  X360DriverManager
//
//  Live visualisation of one controller plus rumble / identify actions.
//

import SwiftUI

struct ControllerTestView: View {
    let pad: ConnectedGamepad

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                if let level = pad.batteryLevel {
                    BatteryLabel(level: level)
                }
            }

            HStack(alignment: .center, spacing: 16) {
                VStack(spacing: 6) {
                    TriggerBar(label: "LT", value: pad.leftTrigger)
                    StickView(label: "L", position: pad.leftStick, pressed: pad.pressed.contains(.leftStick))
                }
                ButtonGrid(pressed: pad.pressed)
                VStack(spacing: 6) {
                    TriggerBar(label: "RT", value: pad.rightTrigger)
                    StickView(label: "R", position: pad.rightStick, pressed: pad.pressed.contains(.rightStick))
                }
            }
            .frame(maxWidth: .infinity)

            HStack {
                Button("Rumble", systemImage: "waveform") {
                    Task { await pad.rumble() }
                }
                Button("Identify", systemImage: "light.max") {
                    Task { await pad.identify() }
                }
                .help("Blinks the ring light of this controller")
            }
        }
        .padding(.vertical, 4)
    }

    private var title: String {
        pad.isWired ? pad.name : "\(pad.name) — Player \(pad.slot + 1)"
    }
}

private struct StickView: View {
    let label: String
    /// -1…1, y positive = up.
    let position: CGPoint
    let pressed: Bool

    private let size: CGFloat = 64

    var body: some View {
        ZStack {
            Circle()
                .strokeBorder(.secondary, lineWidth: 1)
                .background(Circle().fill(pressed ? Color.accentColor.opacity(0.3) : .clear))
            Circle()
                .fill(Color.accentColor)
                .frame(width: 12, height: 12)
                .offset(x: position.x * (size - 12) / 2, y: -position.y * (size - 12) / 2)
        }
        .frame(width: size, height: size)
        .accessibilityElement()
        .accessibilityLabel("\(label) stick")
        .accessibilityValue(String(format: "x %.2f, y %.2f%@", position.x, position.y, pressed ? ", pressed" : ""))
    }
}

private struct TriggerBar: View {
    let label: String
    let value: Double

    var body: some View {
        VStack(spacing: 2) {
            ProgressView(value: value)
                .frame(width: 64)
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
        .accessibilityElement()
        .accessibilityLabel("\(label) trigger")
        .accessibilityValue("\(Int(value * 100)) percent")
    }
}

private struct ButtonGrid: View {
    let pressed: Set<PadButton>

    private let rows: [[PadButton]] = [
        [.leftBumper, .back, .guide, .start, .rightBumper],
        [.dpadUp, .dpadDown, .dpadLeft, .dpadRight],
        [.y, .x, .b, .a],
    ]

    var body: some View {
        Grid(horizontalSpacing: 4, verticalSpacing: 4) {
            ForEach(rows.indices, id: \.self) { row in
                GridRow {
                    ForEach(rows[row]) { button in
                        Text(button.label)
                            .font(.caption.monospaced())
                            .frame(minWidth: 34, minHeight: 20)
                            .background(pressed.contains(button) ? Color.accentColor : Color.secondary.opacity(0.15),
                                        in: .rect(cornerRadius: 4))
                            .foregroundStyle(pressed.contains(button) ? Color.white : Color.primary)
                            .accessibilityLabel(button.label)
                            .accessibilityValue(pressed.contains(button) ? "pressed" : "released")
                    }
                }
            }
        }
    }
}

struct BatteryLabel: View {
    /// 0–255 as reported by the controller.
    let level: Int

    var body: some View {
        let percent = level * 100 / 255
        Label("\(percent) %", systemImage: symbol(for: percent))
            .foregroundStyle(percent < 20 ? .red : .secondary)
    }

    private func symbol(for percent: Int) -> String {
        switch percent {
        case ..<13: "battery.0percent"
        case ..<38: "battery.25percent"
        case ..<63: "battery.50percent"
        case ..<88: "battery.75percent"
        default: "battery.100percent"
        }
    }
}
