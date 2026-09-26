import SwiftUI

@main
struct ScribaticApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @State private var model = TranscriptionModel()

    var body: some Scene {
        WindowGroup {
            RootView(model: model)
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

/// Notes list at the root; the recorder and individual notes push over it.
struct RootView: View {
    @Bindable var model: TranscriptionModel

    var body: some View {
        Group {
            if model.modelsNeeded {
                NavigationStack { ModelSetupView(model: model) }
            } else {
                NavigationStack(path: $model.path) {
                    NotesListView(model: model)
                        .navigationDestination(for: TranscriptionModel.Route.self) { route in
                            switch route {
                            case .recorder:
                                RecorderView(model: model)
                            case let .note(id):
                                NoteDetailView(model: model, noteID: id)
                            }
                        }
                }
            }
        }
        .sheet(isPresented: $model.showingModels) {
            NavigationStack {
                ModelSetupView(model: model)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Done") { model.showingModels = false }
                        }
                    }
            }
        }
        .task { await model.prepare() }
        .alert("Something went wrong", isPresented: errorBinding) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.noteError ?? "")
        }
    }

    private var errorBinding: Binding<Bool> {
        Binding(get: { model.noteError != nil }, set: { if !$0 { model.noteError = nil } })
    }
}
