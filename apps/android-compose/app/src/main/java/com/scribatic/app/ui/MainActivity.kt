package com.scribatic.app.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.text.format.DateFormat
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FabPosition
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LargeTopAppBar
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarColors
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.scribatic.app.R
import com.scribatic.app.engine.NoteDetail
import com.scribatic.app.engine.NoteSummary
import com.scribatic.app.engine.SpeakerLabel
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.ui.theme.Scribatic
import com.scribatic.app.ui.theme.ScribaticTheme
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Date

private fun clock(ms: Long): String = DateUtils.formatElapsedTime(ms / 1000)

private fun speakersLabel(count: Int) = if (count == 1) "1 speaker" else "$count speakers"

/**
 * Single-activity host. The activity owns nothing but the window; all engine
 * state lives in [TranscriptionViewModel] so a configuration change never
 * re-mmaps a multi-hundred-megabyte model.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        // Explicit rather than implicit. From Android 15 an app targeting SDK 35
        // is laid out edge to edge whether it asks or not; declaring it here
        // means older versions behave the same way instead of the layout
        // depending on which OS it lands on.
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        setContent {
            ScribaticTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = Scribatic.colors.paper) {
                    ScribaticApp()
                }
            }
        }
    }
}

@Composable
private fun ScribaticApp(viewModel: TranscriptionViewModel = viewModel()) {
    // State is hoisted out of the engine and observed as an immutable snapshot,
    // so recomposition never blocks on a native call.
    val state by viewModel.uiState.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(Unit) { viewModel.prepare() }

    BackHandler(enabled = state.screen != Screen.Notes) { viewModel.back() }

    // The share sheet. The app itself sends nothing anywhere: this hands text
    // to whichever app the user picks, and that app does the sending — which
    // is why sharing needs no INTERNET permission.
    LaunchedEffect(state.share) {
        val share = state.share ?: return@LaunchedEffect
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, share.subject)
            putExtra(Intent.EXTRA_TEXT, share.text)
        }
        context.startActivity(Intent.createChooser(send, "Share transcript"))
        viewModel.shareHandled()
    }

    state.message?.let { message ->
        AlertDialog(
            onDismissRequest = viewModel::messageShown,
            confirmButton = { TextButton(onClick = viewModel::messageShown) { Text("OK") } },
            text = { Text(message) },
        )
    }

    // safeDrawingPadding keeps content clear of the status bar and the gesture
    // pill. Without it the transport row is drawn underneath the navigation bar
    // and the record button cannot be pressed.
    Box(modifier = Modifier.fillMaxSize().safeDrawingPadding()) {
        when (val screen = state.screen) {
            Screen.Models -> ModelsScreen(state, viewModel)
            Screen.ModelFiles -> ModelFilesScreen(state, viewModel)
            Screen.Notes -> NotesScreen(state, viewModel)
            Screen.Recorder -> RecorderScreen(state, viewModel)
            is Screen.Note -> NoteScreen(screen.id, state, viewModel)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun barColors(): TopAppBarColors {
    val c = Scribatic.colors
    return TopAppBarDefaults.topAppBarColors(
        containerColor = c.paper,
        scrolledContainerColor = c.paper,
        titleContentColor = c.ink,
        navigationIconContentColor = c.ink,
        actionIconContentColor = c.ink,
    )
}

@Composable
private fun BackButton(label: String, onClick: () -> Unit) {
    IconButton(onClick = onClick) { Icon(Icons.back, contentDescription = label) }
}

// -- Notes list ---------------------------------------------------------------------

private data class DayGroup(val title: String, val notes: List<NoteSummary>)

private fun groupByDay(notes: List<NoteSummary>): List<DayGroup> {
    val zone = ZoneId.systemDefault()
    val today = LocalDate.now(zone)
    val thisYear = DateTimeFormatter.ofPattern("EEEE d MMMM")
    val otherYear = DateTimeFormatter.ofPattern("EEEE d MMMM yyyy")
    return notes
        .groupBy { Instant.ofEpochSecond(it.createdAtSeconds).atZone(zone).toLocalDate() }
        .map { (day, dayNotes) ->
            val title = when (day) {
                today -> "Today"
                today.minusDays(1) -> "Yesterday"
                else -> day.format(if (day.year == today.year) thisYear else otherYear)
            }
            DayGroup(title, dayNotes)
        }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NotesScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val c = Scribatic.colors
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    val clipboard = LocalClipboardManager.current
    var pendingDelete by remember { mutableStateOf<NoteSummary?>(null) }
    val stopped = state.failure != null && state.notes.isEmpty()

    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        containerColor = c.paper,
        contentWindowInsets = WindowInsets(0),
        topBar = {
            LargeTopAppBar(
                title = { Text("Notes") },
                actions = {
                    IconButton(onClick = viewModel::openModels) { Icon(Icons.models, contentDescription = "Models") }
                },
                windowInsets = WindowInsets(0),
                colors = barColors(),
                scrollBehavior = scroll,
            )
        },
        floatingActionButton = {
            if (!stopped) {
                // One tap: this opens the recorder, which starts capture.
                RecordButton(
                    stop = false,
                    contentDescription = "New recording",
                    onClick = viewModel::openRecorder,
                    enabled = state.phase == Phase.READY,
                    floating = true,
                )
            }
        },
        floatingActionButtonPosition = FabPosition.Center,
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            when {
                stopped -> EmptyState(
                    title = "Engine stopped",
                    body = state.failure.orEmpty(),
                    failure = true,
                ) {
                    PrimaryButton("Try again", viewModel::prepare, icon = Icons.retry)
                    TextButton(onClick = { clipboard.setText(AnnotatedString(state.failure.orEmpty())) }) {
                        Icon(Icons.copy, contentDescription = null, modifier = Modifier.size(20.dp))
                        Text("Copy details", modifier = Modifier.padding(start = 8.dp))
                    }
                }
                state.phase == Phase.STARTING -> Column(
                    Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterVertically),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    CircularProgressIndicator(color = c.accent, trackColor = c.fillSecondary)
                    Text("Preparing", style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
                }
                state.notes.isEmpty() -> EmptyState(
                    title = "No notes yet",
                    body = "Record a conversation and it appears here, transcribed on this device with each speaker marked.",
                    aside = "Nothing is uploaded.",
                )
                else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 128.dp)) {
                    groupByDay(state.notes).forEach { group ->
                        item(key = "day-${group.title}") {
                            SectionHeader(group.title, if (group.notes.size == 1) "1 note" else "${group.notes.size} notes")
                        }
                        items(group.notes, key = { it.id }) { note ->
                            SwipeableNoteRow(note, onOpen = { viewModel.openNote(note.id) }, onDelete = { pendingDelete = note })
                        }
                    }
                }
            }
        }
    }

    pendingDelete?.let { note ->
        ConfirmDeleteNote(onConfirm = { pendingDelete = null; viewModel.deleteNote(note.id) }, onDismiss = { pendingDelete = null })
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SwipeableNoteRow(note: NoteSummary, onOpen: () -> Unit, onDelete: () -> Unit) {
    val c = Scribatic.colors
    // The swipe asks, it never deletes: the row snaps back and the same
    // confirmation as the note screen decides.
    val swipe = rememberSwipeToDismissBoxState(confirmValueChange = { value ->
        if (value == SwipeToDismissBoxValue.EndToStart) onDelete()
        false
    })
    SwipeToDismissBox(
        state = swipe,
        enableDismissFromStartToEnd = false,
        backgroundContent = {
            Row(
                Modifier.fillMaxSize().background(c.danger).padding(horizontal = 24.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.delete, contentDescription = null, tint = c.onAccent)
                Text("Delete", style = MaterialTheme.typography.labelLarge, color = c.onAccent)
            }
        },
    ) {
        NoteRow(note, onOpen)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun NoteRow(note: NoteSummary, onClick: () -> Unit) {
    val c = Scribatic.colors
    val context = LocalContext.current
    Column(Modifier.fillMaxWidth().background(c.paper)) {
        Column(
            modifier = Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    DateFormat.getTimeFormat(context).format(Date(note.createdAtSeconds * 1000)),
                    style = MaterialTheme.typography.titleMedium,
                    color = c.ink,
                    modifier = Modifier.weight(1f),
                )
                MetaChip(Icons.duration, clock(note.durationMs))
            }
            if (note.preview.isNotEmpty()) {
                Text(
                    note.preview,
                    style = MaterialTheme.typography.bodyMedium,
                    color = c.inkMuted,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (note.speakerCount > 0 || !note.hasAudio) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    if (note.speakerCount > 0) MetaChip(Icons.speakers, speakersLabel(note.speakerCount))
                    // Worth showing in the list: the note can no longer be played
                    // or have its speakers re-identified.
                    if (!note.hasAudio) MetaChip(null, "Audio deleted", painterRes = R.drawable.ic_waveform_slash)
                }
            }
        }
        HorizontalDivider(color = c.line)
    }
}

@Composable
private fun ConfirmDeleteNote(onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Delete this note?") },
        text = { Text("The transcript, the speaker names and the recording are all removed from this device.") },
        confirmButton = {
            TextButton(onClick = onConfirm) { Text("Delete note", color = Scribatic.colors.danger) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// -- Model setup ------------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModelsScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    // Installing from files is deliberately NOT on this screen: next to a
    // download already in progress, a second way to get the same files reads
    // as a choice the user has to make. It has its own screen, offered here
    // only when Play can't provide the models, or chosen from the menu.
    val c = Scribatic.colors
    val context = LocalContext.current
    var confirmLeaveOut by remember { mutableStateOf<ModelRow?>(null) }
    var menu by remember { mutableStateOf(false) }
    val scroll = TopAppBarDefaults.exitUntilCollapsedScrollBehavior()
    // Play's own "download over mobile data?" sheet; the result arrives as pack
    // state updates, so nothing is done with it here.
    val playConfirm = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) {}
    val wantedBytes = state.models.filter { it.wanted }.sumOf { it.spec.sizeBytes }
    val engineRunning = state.phase in setOf(Phase.READY, Phase.RECORDING, Phase.PAUSED, Phase.PROCESSING)

    Scaffold(
        modifier = Modifier.nestedScroll(scroll.nestedScrollConnection),
        containerColor = c.paper,
        contentWindowInsets = WindowInsets(0),
        topBar = {
            LargeTopAppBar(
                title = { Text("Models") },
                navigationIcon = { if (engineRunning) BackButton("Back to notes") { viewModel.back() } },
                actions = {
                    Box {
                        IconButton(onClick = { menu = true }) { Icon(Icons.more, contentDescription = "More options") }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            DropdownMenuItem(
                                text = { Text("Install from files…") },
                                leadingIcon = { Icon(Icons.folder, contentDescription = null) },
                                onClick = {
                                    menu = false
                                    viewModel.openModelFiles()
                                },
                            )
                        }
                    }
                },
                windowInsets = WindowInsets(0),
                colors = barColors(),
                scrollBehavior = scroll,
            )
        },
        bottomBar = {
            Column {
                HorizontalDivider(color = c.line)
                Column(
                    modifier = Modifier.fillMaxWidth().background(c.paper).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    state.playStatus?.let {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            CircularProgressIndicator(modifier = Modifier.size(14.dp), strokeWidth = 2.dp, color = c.accent, trackColor = c.lineStrong)
                            Text(it, style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
                        }
                    }
                    if (state.playNeedsConfirmation) {
                        SecondaryButton("Continue in Google Play", { viewModel.confirmPlayDownload(playConfirm) }, Modifier.fillMaxWidth())
                    }
                    // The fallback, only once the store route has failed.
                    if (state.storeUnavailable) {
                        Text("Google Play download unavailable", style = MaterialTheme.typography.titleSmall, color = c.ink)
                        Text(
                            "The models couldn't be downloaded from Google Play on this phone. You can download " +
                                "them yourself and install them from files instead.",
                            style = MaterialTheme.typography.bodySmall,
                            color = c.inkMuted,
                        )
                        SecondaryButton("Install from files", viewModel::openModelFiles, Modifier.fillMaxWidth(), icon = Icons.folder)
                    }
                    PrimaryButton(
                        "Continue",
                        viewModel::continueFromModels,
                        Modifier.fillMaxWidth(),
                        enabled = state.modelsReady && state.importing == null,
                    )
                }
            }
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            item {
                Text(
                    "Scribatic runs entirely on this phone, so it needs these models. Google Play " +
                        "downloads them for you; the app itself never connects to the internet.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = c.inkMuted,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
                SectionHeader("Models", "${Formatter.formatShortFileSize(context, wantedBytes)} selected")
            }
            item {
                Column(Modifier.padding(horizontal = 16.dp).clip(RoundedCornerShape(12.dp)).background(c.surfaceRaised)) {
                    state.models.forEachIndexed { index, row ->
                        ModelRowView(row) { wanted ->
                            if (wanted) viewModel.setModelWanted(row.spec, true) else confirmLeaveOut = row
                        }
                        if (index < state.models.lastIndex) HorizontalDivider(color = c.line)
                    }
                }
            }
        }
    }

    confirmLeaveOut?.let { row ->
        AlertDialog(
            onDismissRequest = { confirmLeaveOut = null },
            title = { Text("Leave out ${row.spec.title.lowercase()}?") },
            text = {
                Text(
                    row.spec.withoutIt +
                        if (row.installed) " Its ${Formatter.formatShortFileSize(context, row.spec.sizeBytes)} is removed from this phone." else "",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmLeaveOut = null
                    viewModel.setModelWanted(row.spec, false)
                }) { Text("Leave it out", color = c.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmLeaveOut = null }) { Text("Keep it") } },
        )
    }
}

@Composable
private fun ModelRowView(row: ModelRow, onWantedChange: (Boolean) -> Unit) {
    val c = Scribatic.colors
    val context = LocalContext.current
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(row.spec.title, style = MaterialTheme.typography.titleMedium, color = c.ink)
            Text(row.spec.purpose, style = MaterialTheme.typography.bodyMedium, color = c.inkMuted)
            Text(
                "${row.spec.fileName} · ${Formatter.formatShortFileSize(context, row.spec.sizeBytes)}",
                style = MaterialTheme.typography.labelMedium,
                color = c.inkSoft,
            )
            when {
                row.installed -> MetaChip(Icons.installed, "Installed", color = c.success)
                row.wanted -> Text("Needed", style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
                else -> Text("Left out", style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
            }
        }
        if (row.spec.required) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Icon(Icons.lock, contentDescription = null, tint = c.inkMuted, modifier = Modifier.size(14.dp))
                Text("Required", style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
            }
        } else {
            Switch(checked = row.wanted, onCheckedChange = onWantedChange)
        }
    }
}

/**
 * Installing the models from files: download them in a browser from the
 * project's release page, then import them. A separate screen from the Play
 * download on purpose — see [ModelsScreen].
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModelFilesScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val c = Scribatic.colors
    val context = LocalContext.current
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        viewModel.importModels(uris)
    }
    val wanted = state.models.filter { it.wanted }

    Scaffold(
        containerColor = c.paper,
        contentWindowInsets = WindowInsets(0),
        topBar = {
            TopAppBar(
                title = { Text("Install from files") },
                navigationIcon = { BackButton("Back to models") { viewModel.back() } },
                windowInsets = WindowInsets(0),
                colors = barColors(),
            )
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize(), contentPadding = PaddingValues(bottom = 24.dp)) {
            item {
                Text(
                    "Download the model files in your browser, then import them here. Each file is " +
                        "checked before it is used, so its name doesn't matter.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = c.inkMuted,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
                SectionHeader("Steps")
                Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    StepRow(1, "Download model files", "Opens the release page in your browser.", Icons.openInBrowser, c.inkMuted) {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(viewModel.modelDownloadPage())))
                    }
                    StepRow(2, "Import files…", "Pick the downloaded files.", Icons.folder, c.accentStrong, enabled = state.importing == null) {
                        picker.launch(arrayOf("*/*"))
                    }
                    state.importing?.let {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            modifier = Modifier.padding(top = 4.dp),
                        ) {
                            CircularProgressIndicator(modifier = Modifier.size(14.dp), strokeWidth = 2.dp, color = c.accent, trackColor = c.lineStrong)
                            Text(it, style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
                        }
                    }
                }
                SectionHeader("Needed", "${wanted.count { it.installed }} of ${wanted.size} installed")
                Column(Modifier.padding(horizontal = 16.dp).clip(RoundedCornerShape(12.dp)).background(c.surfaceRaised)) {
                    wanted.forEachIndexed { index, row ->
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(row.spec.fileName, style = MaterialTheme.typography.labelMedium, color = c.ink, modifier = Modifier.weight(1f))
                            if (row.installed) {
                                MetaChip(Icons.installed, "Installed", color = c.success)
                            } else {
                                Text(Formatter.formatShortFileSize(context, row.spec.sizeBytes), style = MaterialTheme.typography.labelMedium, color = c.inkMuted)
                            }
                        }
                        if (index < wanted.lastIndex) HorizontalDivider(color = c.line)
                    }
                }
            }
        }
    }
}

@Composable
private fun StepRow(
    number: Int,
    title: String,
    help: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    iconTint: androidx.compose.ui.graphics.Color,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val c = Scribatic.colors
    Surface(onClick = onClick, enabled = enabled, shape = RoundedCornerShape(12.dp), color = c.surfaceRaised) {
        Row(
            Modifier.fillMaxWidth().padding(14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Box(
                Modifier.size(28.dp).clip(CircleShape).background(c.fillSecondary),
                contentAlignment = Alignment.Center,
            ) {
                Text("$number", style = MaterialTheme.typography.labelMedium, color = c.inkMuted)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title, style = MaterialTheme.typography.titleMedium, color = c.ink)
                Text(help, style = MaterialTheme.typography.bodySmall, color = c.inkMuted)
            }
            Icon(icon, contentDescription = null, tint = iconTint)
        }
    }
}

// -- Recorder -----------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RecorderScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val c = Scribatic.colors
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) viewModel.startRecording() }
    val start = {
        val granted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        if (granted) viewModel.startRecording() else permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }
    val listState = rememberLazyListState()
    val capturing = state.phase == Phase.RECORDING || state.phase == Phase.PAUSED || state.phase == Phase.PROCESSING

    // The notes list's record button opens this screen; one tap should be
    // enough to start, so capture begins as soon as it appears.
    LaunchedEffect(Unit) { if (state.phase == Phase.READY) start() }

    LaunchedEffect(state.phase) {
        when (state.phase) {
            Phase.RECORDING, Phase.PROCESSING -> haptics.performHapticFeedback(HapticFeedbackType.LongPress)
            Phase.PAUSED -> haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
            else -> Unit
        }
    }

    // Follow the words as they arrive.
    LaunchedEffect(state.segments.size) {
        if (state.segments.isNotEmpty()) listState.animateScrollToItem(state.segments.lastIndex)
    }

    Scaffold(
        containerColor = c.paper,
        contentWindowInsets = WindowInsets(0),
        topBar = {
            TopAppBar(
                title = { Text("New recording", style = MaterialTheme.typography.titleMedium) },
                // Leaving mid-recording would orphan the capture; Stop saves it.
                navigationIcon = { if (!capturing) BackButton("Back to notes") { viewModel.back() } },
                windowInsets = WindowInsets(0),
                colors = barColors(),
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            Box(modifier = Modifier.weight(1f).fillMaxWidth()) {
                when {
                    state.failure != null -> EmptyState("Engine stopped", state.failure, failure = true)
                    state.segments.isEmpty() -> EmptyState(
                        if (state.phase == Phase.READY) "Ready to record" else "Listening",
                        "Speech is transcribed on this device and appears here — nothing is uploaded, " +
                            "and the microphone is only open while you are recording. When you stop, " +
                            "Scribatic works out who said what.",
                    )
                    else -> LazyColumn(state = listState, contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)) {
                        items(state.segments) { segment -> LiveSegment(segment) }
                    }
                }
            }
            TransportBar(state, viewModel, start)
        }
    }
}

@Composable
private fun LiveSegment(segment: TranscriptSegment) {
    val c = Scribatic.colors
    Column(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(clock(segment.startMs), style = MaterialTheme.typography.labelMedium, color = c.inkSoft)
        Text(segment.text, style = MaterialTheme.typography.bodyLarge, color = c.ink)
    }
}

/**
 * Status and controls, in thumb reach. Which controls exist is driven entirely
 * by the phase, so a button is never shown in a state where it would do nothing.
 */
@Composable
private fun TransportBar(state: TranscriptUiState, viewModel: TranscriptionViewModel, start: () -> Unit) {
    val c = Scribatic.colors
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp))
            .background(c.surfaceRaised)
            .padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            StatusPill(state.phase, state.label)
            Box(Modifier.weight(1f))
            if (state.segments.isNotEmpty()) {
                Text(
                    if (state.segments.size == 1) "1 line" else "${state.segments.size} lines",
                    style = MaterialTheme.typography.labelMedium,
                    color = c.inkMuted,
                )
            }
        }
        when (state.phase) {
            Phase.STARTING, Phase.FAILED, Phase.PROCESSING -> Unit
            Phase.READY -> LabelledControl("Record") {
                RecordButton(stop = false, contentDescription = "Record", onClick = start)
            }
            // Bottom-aligned, so the labels under the two sizes of button share a line.
            Phase.RECORDING, Phase.PAUSED -> Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    if (state.phase == Phase.RECORDING) {
                        LabelledControl("Pause") { RoundButton(Icons.pause, "Pause", viewModel::pauseRecording) }
                    } else {
                        LabelledControl("Resume") { RoundButton(Icons.play, "Resume", viewModel::resumeRecording) }
                    }
                }
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    LabelledControl("Stop") {
                        RecordButton(stop = true, contentDescription = "Stop and save", onClick = viewModel::stopRecording)
                    }
                }
                Box(Modifier.weight(1f))
            }
        }
    }
}

// -- Note -----------------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun NoteScreen(id: Long, state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val c = Scribatic.colors
    val note = state.note
    var menu by remember { mutableStateOf(false) }
    var shareMenu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<SpeakerLabel?>(null) }
    var confirmDeleteRecording by remember { mutableStateOf(false) }
    var confirmDeleteNote by remember { mutableStateOf(false) }
    var identifyMenu by remember { mutableStateOf(false) }

    Scaffold(
        containerColor = c.paper,
        contentWindowInsets = WindowInsets(0),
        topBar = {
            TopAppBar(
                title = {},
                navigationIcon = { BackButton("Back to notes") { viewModel.back() } },
                actions = {
                    if (note != null) {
                        Box {
                            IconButton(
                                enabled = !state.busy,
                                onClick = {
                                    // Only one way to share: skip the menu.
                                    if (note.speakerCount > 0) shareMenu = true else viewModel.share(id, anonymise = false)
                                },
                            ) { Icon(Icons.share, contentDescription = "Share") }
                            DropdownMenu(expanded = shareMenu, onDismissRequest = { shareMenu = false }) {
                                DropdownMenuItem(
                                    text = { Text("Share transcript") },
                                    leadingIcon = { Icon(Icons.share, contentDescription = null) },
                                    onClick = {
                                        shareMenu = false
                                        viewModel.share(id, anonymise = false)
                                    },
                                )
                                DropdownMenuItem(
                                    text = { Text("Share without names") },
                                    leadingIcon = { Icon(Icons.shareWithoutNames, contentDescription = null) },
                                    onClick = {
                                        shareMenu = false
                                        viewModel.share(id, anonymise = true)
                                    },
                                )
                            }
                        }
                        Box {
                            IconButton(enabled = !state.busy, onClick = { menu = true }) {
                                Icon(Icons.more, contentDescription = "More")
                            }
                            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                                val danger = MenuDefaults.itemColors(textColor = c.danger, leadingIconColor = c.danger)
                                if (note.audioPath != null && state.canIdentifySpeakers) {
                                    DropdownMenuItem(
                                        text = { Text("Identify speakers again…") },
                                        leadingIcon = { Icon(Icons.identifySpeakers, contentDescription = null) },
                                        onClick = {
                                            menu = false
                                            identifyMenu = true
                                        },
                                    )
                                    HorizontalDivider(color = c.line)
                                }
                                if (note.audioPath != null) {
                                    DropdownMenuItem(
                                        text = { Text("Delete recording…") },
                                        leadingIcon = { Icon(painterResource(R.drawable.ic_waveform_slash), contentDescription = null) },
                                        colors = danger,
                                        onClick = {
                                            menu = false
                                            confirmDeleteRecording = true
                                        },
                                    )
                                }
                                DropdownMenuItem(
                                    text = { Text("Delete note…") },
                                    leadingIcon = { Icon(Icons.delete, contentDescription = null) },
                                    colors = danger,
                                    onClick = {
                                        menu = false
                                        confirmDeleteNote = true
                                    },
                                )
                            }
                        }
                    }
                },
                windowInsets = WindowInsets(0),
                colors = barColors(),
            )
        },
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            if (note == null) {
                CircularProgressIndicator(Modifier.align(Alignment.Center), color = c.accent, trackColor = c.fillSecondary)
            } else {
                NoteBody(
                    note = note,
                    busy = state.busy,
                    playing = state.playingNoteId == id,
                    onPlay = { if (state.playingNoteId == id) viewModel.stopPlayback() else viewModel.play(note) },
                    onRename = { renaming = it },
                )
            }
        }
    }

    renaming?.let { speaker ->
        var name by remember(speaker) { mutableStateOf(speaker.name) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            title = { Text("Name this speaker") },
            text = {
                Column {
                    Text(
                        "Only on this device, and only for this note. Leave it empty to go back to the numbered label.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    OutlinedTextField(
                        value = name,
                        onValueChange = { name = it },
                        placeholder = { Text(speaker.displayName) },
                        singleLine = true,
                        modifier = Modifier.padding(top = 12.dp),
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    renaming = null
                    viewModel.renameSpeaker(id, speaker.index, name)
                }) { Text("Save") }
            },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel") } },
        )
    }

    if (identifyMenu) {
        AlertDialog(
            onDismissRequest = { identifyMenu = false },
            title = { Text("How many people were talking?") },
            text = {
                Column {
                    (listOf(0) + (2..6)).forEach { count ->
                        TextButton(onClick = {
                            identifyMenu = false
                            viewModel.identifySpeakers(id, count)
                        }) { Text(if (count == 0) "Work it out" else "$count people") }
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { identifyMenu = false }) { Text("Cancel") } },
        )
    }

    if (confirmDeleteRecording) {
        AlertDialog(
            onDismissRequest = { confirmDeleteRecording = false },
            title = { Text("Delete the recording?") },
            text = {
                Text(
                    "The transcript and speaker names stay. The audio is removed from this device and " +
                        "cannot be played again, and speakers can no longer be re-identified.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmDeleteRecording = false
                    viewModel.deleteRecording(id)
                }) { Text("Delete recording", color = c.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmDeleteRecording = false }) { Text("Cancel") } },
        )
    }

    if (confirmDeleteNote) {
        ConfirmDeleteNote(
            onConfirm = {
                confirmDeleteNote = false
                viewModel.deleteNote(id)
            },
            onDismiss = { confirmDeleteNote = false },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun NoteBody(
    note: NoteDetail,
    busy: Boolean,
    playing: Boolean,
    onPlay: () -> Unit,
    onRename: (SpeakerLabel) -> Unit,
) {
    val c = Scribatic.colors
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 24.dp)) {
        item {
            Column(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    note.title,
                    style = MaterialTheme.typography.headlineSmall,
                    color = c.ink,
                    modifier = Modifier.semantics { heading() },
                )
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    MetaChip(Icons.duration, clock(note.durationMs))
                    if (note.speakerCount > 0) MetaChip(Icons.speakers, speakersLabel(note.speakerCount))
                    if (note.audioPath != null) {
                        MetaBadge(Icons.lock, "On this phone")
                    } else {
                        MetaChip(null, "Audio deleted", painterRes = R.drawable.ic_waveform_slash)
                    }
                }
                if (busy) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        CircularProgressIndicator(modifier = Modifier.size(14.dp), strokeWidth = 2.dp, color = c.accent, trackColor = c.lineStrong)
                        Text("Working…", style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
                    }
                }
            }
        }

        if (note.audioPath != null) {
            item {
                Row(
                    Modifier
                        .padding(horizontal = 16.dp)
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(12.dp))
                        .background(c.surfaceRaised)
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Surface(onClick = onPlay, shape = CircleShape, color = c.ink, contentColor = c.paper, modifier = Modifier.size(44.dp)) {
                        Box(contentAlignment = Alignment.Center) {
                            Icon(
                                if (playing) Icons.stop else Icons.play,
                                contentDescription = if (playing) "Stop playback" else "Play recording",
                                modifier = Modifier.size(26.dp),
                            )
                        }
                    }
                    Column {
                        Text(if (playing) "Playing" else "Play recording", style = MaterialTheme.typography.labelLarge, color = c.ink)
                        Text(clock(note.durationMs), style = MaterialTheme.typography.labelMedium, color = c.inkMuted)
                    }
                }
            }
        }

        if (note.speakers.isNotEmpty()) {
            item {
                SectionHeader("Speakers")
                FlowRow(
                    modifier = Modifier.padding(horizontal = 16.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    note.speakers.forEach { speaker ->
                        SpeakerChip(speaker.displayName, c.speaker(speaker.index)) { onRename(speaker) }
                    }
                }
                Text(
                    "Tap to name. If one person was split into two, give both the same name.",
                    style = MaterialTheme.typography.bodySmall,
                    color = c.inkMuted,
                    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 8.dp),
                )
            }
        }

        item { SectionHeader("Transcript") }
        if (note.segments.isEmpty()) {
            item {
                Text(
                    "Nothing was transcribed.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = c.inkMuted,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }
        }
        itemsIndexed(note.segments) { index, segment ->
            // A name only where the speaker changes: a run of one person's
            // sentences reads as a paragraph.
            val showsSpeaker = index == 0 || note.segments[index - 1].speaker != segment.speaker
            SegmentRow(segment, note.speakerName(segment.speaker), showsSpeaker)
        }
    }
}

@Composable
private fun SegmentRow(segment: TranscriptSegment, speakerName: String?, showsSpeaker: Boolean) {
    val c = Scribatic.colors
    Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
        if (showsSpeaker) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.padding(top = 8.dp, bottom = 2.dp),
            ) {
                if (speakerName != null) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(c.speaker(segment.speaker)))
                    Text(
                        speakerName,
                        color = c.speaker(segment.speaker),
                        fontWeight = FontWeight.SemiBold,
                        style = MaterialTheme.typography.labelLarge,
                        modifier = Modifier.weight(1f),
                    )
                } else {
                    Box(Modifier.weight(1f))
                }
                Text(clock(segment.startMs), style = MaterialTheme.typography.labelMedium, color = c.inkSoft)
            }
        }
        Text(segment.text, style = MaterialTheme.typography.bodyLarge, color = c.ink)
    }
}
