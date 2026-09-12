package com.scribatic.app.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

/**
 * Single-activity host. The activity owns nothing but the window; all engine
 * state lives in [TranscriptionViewModel] so a configuration change never
 * re-mmaps a multi-hundred-megabyte model.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
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

    Column(modifier = Modifier.padding(16.dp)) {
        Text(text = uiState.statusLabel, style = MaterialTheme.typography.labelMedium)
        uiState.segments.forEach { segment ->
            Text(text = segment.text, style = MaterialTheme.typography.bodyLarge)
        }
    }
}
