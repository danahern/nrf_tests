import SwiftUI

struct ContentView: View {
    var ble: BLEManager

    var body: some View {
        VStack(spacing: 0) {
            // Status banner
            Text(ble.status)
                .font(.headline)
                .frame(maxWidth: .infinity)
                .padding()
                .background(statusColor.opacity(0.15))
                .foregroundStyle(statusColor)

            // Throughput readout
            VStack(spacing: 4) {
                Text(String(format: "%.0f", ble.currentKbps))
                    .font(.system(size: 72, weight: .bold, design: .monospaced))
                    .contentTransition(.numericText())
                Text("kbps")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }
            .padding(.vertical, 24)

            // Stats grid
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                StatCell(label: "Avg", value: String(format: "%.0f kbps", ble.averageKbps))
                StatCell(label: "Total", value: formatBytes(ble.totalBytes))
                StatCell(label: "Elapsed", value: formatTime(ble.elapsed))
            }
            .padding(.horizontal)

            Divider().padding(.vertical, 12)

            // Log
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(ble.logMessages.enumerated()), id: \.offset) { i, msg in
                            Text(msg)
                                .font(.caption.monospaced())
                                .id(i)
                        }
                    }
                    .padding(.horizontal)
                }
                .onChange(of: ble.logMessages.count) { _, newCount in
                    if newCount > 0 { proxy.scrollTo(newCount - 1, anchor: .bottom) }
                }
            }

            Spacer(minLength: 0)

            // Button
            Button(action: {
                if ble.isConnected { ble.disconnect() } else { ble.start() }
            }) {
                Text(ble.isConnected ? "Disconnect" : "Start")
                    .font(.headline)
                    .frame(maxWidth: .infinity)
                    .padding()
            }
            .buttonStyle(.borderedProminent)
            .tint(ble.isConnected ? .red : .blue)
            .padding()
        }
    }

    private var statusColor: Color {
        switch ble.status {
        case "Streaming": .green
        case let s where s.contains("error") || s.contains("failed") || s.contains("timeout"): .red
        case "Disconnected", "Idle": .secondary
        default: .orange
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1_048_576 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / 1_048_576)
    }

    private func formatTime(_ t: TimeInterval) -> String {
        let m = Int(t) / 60
        let s = Int(t) % 60
        return String(format: "%d:%02d", m, s)
    }
}

private struct StatCell: View {
    let label: String
    let value: String

    var body: some View {
        VStack(spacing: 2) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.subheadline.monospaced())
        }
    }
}
