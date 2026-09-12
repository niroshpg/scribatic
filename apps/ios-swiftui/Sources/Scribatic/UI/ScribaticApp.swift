import SwiftUI

@main
struct ScribaticApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @State private var model = TranscriptionModel()

    var body: some Scene {
        WindowGroup {
            TranscriptView(model: model)
        }
        .onChange(of: scenePhase) { _, phase in
            // Releasing the weight mapping on backgrounding keeps the process
            // footprint well under the jetsam limit for its band.
            if phase == .background {
                Task { await model.hibernate() }
            }
        }
    }
}
