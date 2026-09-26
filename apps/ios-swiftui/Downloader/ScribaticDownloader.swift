import BackgroundAssets
import ExtensionFoundation

/// The Background Assets downloader for Apple-hosted asset packs (ADR-011).
///
/// The system runs this extension to download the model packs from Apple's
/// servers — at install, per each pack's download policy, or when the app asks
/// for one. The managed protocol supplies the whole implementation; this app
/// accepts every pack it is offered. There is no network code here either:
/// Apple hosts the packs and the system fetches them.
@main
struct ScribaticDownloader: ManagedDownloaderExtension {
    func shouldDownload(_ assetPack: AssetPack) -> Bool {
        true
    }
}
