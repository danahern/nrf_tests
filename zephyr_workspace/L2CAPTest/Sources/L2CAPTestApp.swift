import SwiftUI

@main
struct L2CAPTestApp: App {
    @State private var bleManager = BLEManager()

    var body: some Scene {
        WindowGroup {
            ContentView(ble: bleManager)
        }
    }
}
