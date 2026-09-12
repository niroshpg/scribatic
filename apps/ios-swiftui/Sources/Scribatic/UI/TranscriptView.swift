import SwiftUI

struct TranscriptView: View {
    @Bindable var model: TranscriptionModel

    var body: some View {
        NavigationStack {
            List(model.segments) { segment in
                Text(segment.text)
                    .font(.body)
                    .foregroundStyle(segment.isFinal ? .primary : .secondary)
            }
            .navigationTitle("Transcript")
            .toolbar {
                ToolbarItem(placement: .status) {
                    Text(model.statusLabel).font(.caption)
                }
            }
            .task { await model.start() }
        }
    }
}
