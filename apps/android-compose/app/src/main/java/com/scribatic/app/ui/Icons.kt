package com.scribatic.app.ui

import androidx.compose.material.icons.Icons as MaterialIcons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.Edit
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Group
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Mic
import androidx.compose.material.icons.rounded.MoreVert
import androidx.compose.material.icons.rounded.NoAccounts
import androidx.compose.material.icons.rounded.OpenInBrowser
import androidx.compose.material.icons.rounded.Pause
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.RecordVoiceOver
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Schedule
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material.icons.rounded.Warning

/**
 * The app's icon set: Material Icons, Rounded style, the Compose-packaged
 * match for the design system's Material Symbols Rounded. One name per
 * meaning, matching the SF Symbols the iOS app uses (see the design-system
 * iconography table). The custom "audio deleted" glyph is
 * R.drawable.ic_waveform_slash.
 */
object Icons {
    val mic = MaterialIcons.Rounded.Mic
    val stop = MaterialIcons.Rounded.Stop
    val pause = MaterialIcons.Rounded.Pause
    val play = MaterialIcons.Rounded.PlayArrow
    val share = MaterialIcons.Rounded.Share
    val shareWithoutNames = MaterialIcons.Rounded.NoAccounts
    val more = MaterialIcons.Rounded.MoreVert
    val edit = MaterialIcons.Rounded.Edit
    val identifySpeakers = MaterialIcons.Rounded.RecordVoiceOver
    val speakers = MaterialIcons.Rounded.Group
    val duration = MaterialIcons.Rounded.Schedule
    val delete = MaterialIcons.Rounded.Delete
    val models = MaterialIcons.Rounded.Memory
    val folder = MaterialIcons.Rounded.FolderOpen
    val openInBrowser = MaterialIcons.Rounded.OpenInBrowser
    val installed = MaterialIcons.Rounded.CheckCircle
    val lock = MaterialIcons.Rounded.Lock
    val warning = MaterialIcons.Rounded.Warning
    val copy = MaterialIcons.Rounded.ContentCopy
    val retry = MaterialIcons.Rounded.Refresh
    val back = MaterialIcons.AutoMirrored.Rounded.ArrowBack
    val language = MaterialIcons.Rounded.Translate
}
