package com.scribatic.app.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.text.format.DateUtils
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
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.scribatic.app.engine.NoteDetail
import com.scribatic.app.engine.NoteSummary
import com.scribatic.app.engine.SpeakerLabel
import com.scribatic.app.engine.TranscriptSegment

/** Atomic tangerine — the accent the diagrams, docs and scribatic.com all use. */
private val Accent = Color(0xFFEB6C36)

/** One colour per speaker. Never the only signal: the name is always shown. */
private val SpeakerColors = listOf(
    Accent, Color(0xFF1E6FD9), Color(0xFF2E9E5B), Color(0xFF8E4EC6),
    Color(0xFFD6457A), Color(0xFF1B9AAA), Color(0xFF5B5BD6), Color(0xFF8A6A4F),
)

private fun speakerColor(index: Int): Color =
    if (index < 0) Color.Gray else SpeakerColors[index % SpeakerColors.size]

private fun clock(ms: Long): String = DateUtils.formatElapsedTime(ms / 1000)

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
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
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
            Screen.Notes -> NotesScreen(state, viewModel)
            Screen.Recorder -> RecorderScreen(state, viewModel)
            is Screen.Note -> NoteScreen(screen.id, state, viewModel)
        }
    }
}

// -- Notes list ---------------------------------------------------------------------

@Composable
private fun NotesScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Notes", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.weight(1f))
            Button(
                onClick = viewModel::openRecorder,
                enabled = state.phase == Phase.READY,
                colors = ButtonDefaults.buttonColors(containerColor = Accent),
            ) { Text("New recording") }
        }

        when {
            state.failure != null && state.notes.isEmpty() ->
                CenteredMessage("Engine stopped", state.failure)
            state.phase == Phase.STARTING -> CenteredMessage("Preparing", "Loading the on-device models.")
            state.notes.isEmpty() -> CenteredMessage(
                "No notes yet",
                "Record a conversation and it appears here, transcribed on this device with each speaker marked.",
            )
            else -> LazyColumn(modifier = Modifier.fillMaxSize()) {
                items(state.notes, key = { it.id }) { note ->
                    NoteRow(note) { viewModel.openNote(note.id) }
                    HorizontalDivider()
                }
            }
        }
    }
}

@Composable
private fun NoteRow(note: NoteSummary, onClick: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick).padding(16.dp),
    ) {
        Text(note.title, style = MaterialTheme.typography.titleMedium)
        if (note.preview.isNotEmpty()) {
            Text(
                note.preview,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        val details = buildList {
            add(clock(note.durationMs))
            if (note.speakerCount > 0) add("${note.speakerCount} speakers")
            // Worth showing in the list: the note can no longer be played or
            // have its speakers re-identified.
            if (!note.hasAudio) add("Audio deleted")
        }
        Text(
            details.joinToString(" · "),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.outline,
            modifier = Modifier.padding(top = 4.dp),
        )
    }
}

// -- Recorder -----------------------------------------------------------------------

@Composable
private fun RecorderScreen(state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val context = LocalContext.current
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) viewModel.startRecording() }
    val listState = rememberLazyListState()

    // Follow the words as they arrive.
    LaunchedEffect(state.segments.size) {
        if (state.segments.isNotEmpty()) listState.animateScrollToItem(state.segments.lastIndex)
    }

    Column(modifier = Modifier.fillMaxSize()) {
        Text("New recording", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.padding(16.dp))

        Box(modifier = Modifier.weight(1f).fillMaxWidth()) {
            when {
                state.failure != null -> CenteredMessage("Engine stopped", state.failure)
                state.segments.isEmpty() -> CenteredMessage(
                    if (state.phase == Phase.READY) "Ready to record" else "Listening",
                    "Speech is transcribed on this device and appears here — nothing is uploaded, " +
                        "and the microphone is only open while you are recording. When you stop, " +
                        "Scribatic works out who said what.",
                )
                else -> LazyColumn(state = listState, modifier = Modifier.padding(horizontal = 16.dp)) {
                    items(state.segments) { segment ->
                        Text(
                            text = segment.text,
                            style = MaterialTheme.typography.bodyLarge,
                            modifier = Modifier.padding(vertical = 4.dp),
                        )
                    }
                }
            }
        }

        HorizontalDivider()

        // Which controls exist is driven entirely by the phase, so a button is
        // never shown in a state where it would do nothing.
        Column(modifier = Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (state.phase == Phase.PROCESSING) {
                    CircularProgressIndicator(modifier = Modifier.size(14.dp), strokeWidth = 2.dp)
                } else {
                    Box(
                        modifier = Modifier.size(10.dp).clip(CircleShape).background(
                            when (state.phase) {
                                Phase.RECORDING -> Accent
                                Phase.FAILED -> MaterialTheme.colorScheme.error
                                else -> MaterialTheme.colorScheme.outline
                            },
                        ),
                    )
                }
                Text(state.label, style = MaterialTheme.typography.labelLarge, modifier = Modifier.padding(start = 8.dp))
                Box(modifier = Modifier.weight(1f))
                if (state.segments.isNotEmpty()) {
                    Text(
                        "${state.segments.size} segments",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.outline,
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally),
            ) {
                when (state.phase) {
                    Phase.STARTING, Phase.FAILED, Phase.PROCESSING -> Unit

                    Phase.READY -> Button(
                        onClick = {
                            val granted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
                                PackageManager.PERMISSION_GRANTED
                            if (granted) viewModel.startRecording()
                            else permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = Accent),
                    ) { Text("Record") }

                    Phase.RECORDING -> {
                        OutlinedButton(onClick = viewModel::pauseRecording) { Text("Pause") }
                        OutlinedButton(onClick = viewModel::stopRecording) { Text("Stop") }
                    }

                    Phase.PAUSED -> {
                        OutlinedButton(onClick = viewModel::resumeRecording) { Text("Resume") }
                        OutlinedButton(onClick = viewModel::stopRecording) { Text("Stop") }
                    }
                }
            }
        }
    }
}

// -- Note -----------------------------------------------------------------------------

@Composable
private fun NoteScreen(id: Long, state: TranscriptUiState, viewModel: TranscriptionViewModel) {
    val note = state.note
    var menu by remember { mutableStateOf(false) }
    var shareMenu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<SpeakerLabel?>(null) }
    var confirmDeleteRecording by remember { mutableStateOf(false) }
    var confirmDeleteNote by remember { mutableStateOf(false) }
    var identifyMenu by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = { viewModel.back() }) { Text("Notes") }
            Text(
                note?.title.orEmpty(),
                style = MaterialTheme.typography.titleMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            if (note != null) {
                Box {
                    TextButton(onClick = { shareMenu = true }, enabled = !state.busy) { Text("Share") }
                    DropdownMenu(expanded = shareMenu, onDismissRequest = { shareMenu = false }) {
                        DropdownMenuItem(text = { Text("Share transcript") }, onClick = {
                            shareMenu = false
                            viewModel.share(id, anonymise = false)
                        })
                        if (note.speakerCount > 0) {
                            DropdownMenuItem(text = { Text("Share without names") }, onClick = {
                                shareMenu = false
                                viewModel.share(id, anonymise = true)
                            })
                        }
                    }
                }
                Box {
                    TextButton(onClick = { menu = true }, enabled = !state.busy) { Text("More") }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        if (note.audioPath != null) {
                            if (state.playingNoteId == id) {
                                DropdownMenuItem(text = { Text("Stop playback") }, onClick = {
                                    menu = false
                                    viewModel.stopPlayback()
                                })
                            } else {
                                DropdownMenuItem(text = { Text("Play recording") }, onClick = {
                                    menu = false
                                    viewModel.play(note)
                                })
                            }
                            if (state.canIdentifySpeakers) {
                                DropdownMenuItem(text = { Text("Identify speakers again") }, onClick = {
                                    menu = false
                                    identifyMenu = true
                                })
                            }
                            DropdownMenuItem(text = { Text("Delete recording…") }, onClick = {
                                menu = false
                                confirmDeleteRecording = true
                            })
                        }
                        DropdownMenuItem(text = { Text("Delete note…") }, onClick = {
                            menu = false
                            confirmDeleteNote = true
                        })
                    }
                }
            }
        }
        HorizontalDivider()

        when {
            note == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
            else -> NoteBody(note, busy = state.busy, onRename = { renaming = it })
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
                }) { Text("Delete recording", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { confirmDeleteRecording = false }) { Text("Cancel") } },
        )
    }

    if (confirmDeleteNote) {
        AlertDialog(
            onDismissRequest = { confirmDeleteNote = false },
            title = { Text("Delete this note?") },
            text = { Text("The transcript, the speaker names and the recording are all removed from this device.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmDeleteNote = false
                    viewModel.deleteNote(id)
                }) { Text("Delete note", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { confirmDeleteNote = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun NoteBody(note: NoteDetail, busy: Boolean, onRename: (SpeakerLabel) -> Unit) {
    LazyColumn(modifier = Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
        item {
            Text(
                "Length ${clock(note.durationMs)} · " +
                    if (note.audioPath != null) "Recording on this device" else "Recording deleted",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 12.dp),
            )
            if (busy) {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = 12.dp)) {
                    CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                    Text("Working…", modifier = Modifier.padding(start = 8.dp))
                }
            }
        }

        if (note.speakers.isNotEmpty()) {
            item { SectionHeader("Speakers") }
            items(note.speakers, key = { "speaker-${it.index}" }) { speaker ->
                Row(
                    modifier = Modifier.fillMaxWidth().clickable { onRename(speaker) }.padding(vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.size(10.dp).clip(CircleShape).background(speakerColor(speaker.index)))
                    Text(speaker.displayName, modifier = Modifier.padding(start = 12.dp).weight(1f))
                    Text("Rename", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.labelLarge)
                }
            }
            item {
                Text(
                    "Tap to name. If one person was split into two, give both the same name.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.outline,
                )
            }
        }

        item { SectionHeader("Transcript") }
        if (note.segments.isEmpty()) {
            item { Text("Nothing was transcribed.", color = MaterialTheme.colorScheme.outline) }
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
private fun SectionHeader(title: String) {
    Text(
        title.uppercase(),
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.outline,
        modifier = Modifier.padding(top = 16.dp, bottom = 4.dp),
    )
}

@Composable
private fun SegmentRow(segment: TranscriptSegment, speakerName: String?, showsSpeaker: Boolean) {
    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        if (showsSpeaker) {
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 8.dp)) {
                if (speakerName != null) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(speakerColor(segment.speaker)))
                    Text(
                        speakerName,
                        color = speakerColor(segment.speaker),
                        fontWeight = FontWeight.SemiBold,
                        style = MaterialTheme.typography.labelLarge,
                        modifier = Modifier.padding(start = 6.dp).weight(1f),
                    )
                } else {
                    Box(Modifier.weight(1f))
                }
                Text(clock(segment.startMs), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.outline)
            }
        }
        Text(segment.text, style = MaterialTheme.typography.bodyLarge)
    }
}

@Composable
private fun CenteredMessage(title: String, body: String) {
    Column(
        modifier = Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(text = title, style = MaterialTheme.typography.titleMedium)
        Text(
            text = body,
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 8.dp),
        )
    }
}
