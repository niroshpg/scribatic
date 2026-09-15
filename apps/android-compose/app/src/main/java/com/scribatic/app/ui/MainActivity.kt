package com.scribatic.app.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.compose.foundation.background

/** Atomic tangerine — the accent the diagrams, docs and scribatic.com all use. */
private val Accent = Color(0xFFEB6C36)

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
                    TranscriptScreen()
                }
            }
        }
    }
}

@Composable
private fun TranscriptScreen(viewModel: TranscriptionViewModel = viewModel()) {
    // State is hoisted out of the engine and observed as an immutable snapshot,
    // so recomposition never blocks on a native call.
    val uiState by viewModel.uiState.collectAsState()
    val context = androidx.compose.ui.platform.LocalContext.current

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) viewModel.startRecording() }

    LaunchedEffect(Unit) { viewModel.prepare() }

    // safeDrawingPadding keeps content clear of the status bar and the gesture
    // pill. Without it the transport row is drawn underneath the navigation bar
    // and the record button cannot be pressed. The Surface still paints edge to
    // edge behind them, which is the point of going edge to edge at all.
    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding(),
    ) {
        Text(
            text = "Transcript",
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.padding(16.dp),
        )

        Box(modifier = Modifier.weight(1f).fillMaxWidth()) {
            when {
                uiState.failure != null -> CenteredMessage(
                    title = "Engine stopped",
                    body = uiState.failure!!,
                )

                uiState.segments.isEmpty() -> CenteredMessage(
                    title = "Nothing transcribed yet",
                    body = "Press record to start. Speech is transcribed on this " +
                        "device and appears here — nothing is uploaded, and the " +
                        "microphone is only open while you are recording.",
                )

                else -> LazyColumn(modifier = Modifier.padding(horizontal = 16.dp)) {
                    items(uiState.segments) { segment ->
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
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .clip(CircleShape)
                        .background(
                            when (uiState.phase) {
                                Phase.RECORDING -> Accent
                                Phase.PLAYING -> MaterialTheme.colorScheme.primary
                                Phase.FAILED -> MaterialTheme.colorScheme.error
                                else -> MaterialTheme.colorScheme.outline
                            },
                        ),
                )
                Text(
                    text = uiState.label,
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.padding(start = 8.dp),
                )
                Box(modifier = Modifier.weight(1f))
                if (uiState.segments.isNotEmpty()) {
                    Text(
                        text = "${uiState.segments.size} segments",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.outline,
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally),
            ) {
                when (uiState.phase) {
                    Phase.STARTING, Phase.FAILED -> Unit

                    Phase.READY -> {
                        Button(
                            onClick = {
                                val granted = context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
                                    PackageManager.PERMISSION_GRANTED
                                if (granted) viewModel.startRecording()
                                else permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
                            },
                            colors = ButtonDefaults.buttonColors(containerColor = Accent),
                        ) { Text("Record") }

                        if (uiState.hasRecording) {
                            OutlinedButton(onClick = viewModel::play) { Text("Play") }
                        }
                        // Sits beside the other controls rather than as a faint
                        // text link in the corner, which was easy to miss.
                        if (uiState.canClear) {
                            OutlinedButton(onClick = viewModel::clear) { Text("Delete") }
                        }
                    }

                    Phase.RECORDING -> {
                        OutlinedButton(onClick = viewModel::pauseRecording) { Text("Pause") }
                        OutlinedButton(onClick = viewModel::stopRecording) { Text("Stop") }
                    }

                    Phase.PLAYING -> OutlinedButton(onClick = viewModel::stopPlayback) {
                        Text("Stop")
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
