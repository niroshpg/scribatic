import SwiftUI

/// Add-ons: the paid features, which live in a private repository cloned into
/// pro/ at the root of this one. When it is there, project.yml includes its
/// spec, which compiles its sources into the app and sets SCRIBATIC_PRO, and
/// `install` registers them; when it is not — any public checkout — there are
/// no add-ons and the app is complete without them.
///
/// Nothing here knows what an add-on does. It gets a place on the note screen
/// and the notes list, the engine, and the models, through `ExtensionHost`.
@MainActor
enum Extensions {
    /// Sections add-ons put on the note screen, under the player.
    static var noteSections: [any NoteSection] = []

    /// Screens add-ons offer from the notes list, each behind a toolbar button.
    static var screens: [any ExtensionScreen] = []

    /// Optional models an installed add-on uses, by file name. Only these are
    /// offered in Models: without the add-on that uses it, a model would be a
    /// download that does nothing.
    static var optionalModels: Set<String> = []

    private static var installed = false

    /// Called once, before the models are listed.
    static func install() {
        guard !installed else { return }
        installed = true
        #if SCRIBATIC_PRO
        ProExtensions.install()
        #endif
    }
}

/// A section on the note screen.
@MainActor
protocol NoteSection {
    func content(note: NoteDetailValue, host: any ExtensionHost) -> AnyView
}

/// A screen of an add-on's own, pushed from the notes list.
@MainActor
protocol ExtensionScreen {
    /// For the button's label and the screen's title.
    var title: String { get }
    var systemImage: String { get }
    func content(host: any ExtensionHost) -> AnyView
}

/// What an add-on can see of the app and ask of it. TranscriptionModel is it.
@MainActor
protocol ExtensionHost: AnyObject {
    /// The running engine, or nil before it has started.
    var runningEngine: ScribaticEngine? { get }

    var notes: [NoteSummaryValue] { get }

    /// The models setup lists, installed or not.
    var models: [TranscriptionModel.ModelRow] { get }

    /// "Downloading models — 40%" while a pack downloads.
    var packStatus: String? { get }

    /// Where a model's file is, if it is on the device.
    func modelURL(_ fileName: String) -> URL?

    /// The note's transcript or speakers are being rebuilt (finishing after
    /// Stop, or identifying speakers again): anything read from it now is
    /// read half-done.
    func isRebuilding(_ noteID: Int64) -> Bool

    /// Asks for an optional model; the App Store downloads its pack.
    func requestModel(_ fileName: String)

    /// Re-reads the notes and any open one, e.g. after an add-on changed a
    /// note through the engine.
    func reloadNotes()

    /// Opens a note, scrolled to the part at `atMs` in its recording and
    /// marked for a moment. Back returns to the screen that opened it.
    func openNote(_ id: Int64, atMs: Int64)

    /// A failure to tell the user about.
    func showError(_ text: String)
}
