package com.scribatic.app.ui

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import android.provider.Settings
import com.scribatic.app.R
import com.scribatic.app.ui.theme.Scribatic

/** One primary action per screen: accent-strong fill, on-accent label. */
@Composable
fun PrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
) {
    val c = Scribatic.colors
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.heightIn(min = 50.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = c.accentStrong, contentColor = c.onAccent,
            disabledContainerColor = c.accentStrong.copy(alpha = 0.38f), disabledContentColor = c.onAccent,
        ),
        contentPadding = PaddingValues(horizontal = 22.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
            Box(Modifier.size(8.dp))
        }
        Text(text, style = MaterialTheme.typography.titleMedium)
    }
}

/** Alternatives beside a primary: fill-secondary ground, ink label. */
@Composable
fun SecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
) {
    val c = Scribatic.colors
    FilledTonalButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.heightIn(min = 50.dp),
        colors = ButtonDefaults.filledTonalButtonColors(containerColor = c.fillSecondary, contentColor = c.ink),
        contentPadding = PaddingValues(horizontal = 22.dp),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
            Box(Modifier.size(8.dp))
        }
        Text(text, style = MaterialTheme.typography.titleMedium)
    }
}

/**
 * The round record / stop control: the one element that uses the brand
 * accent as a fill. Stop shows a rounded square with a ring outside it.
 */
@Composable
fun RecordButton(
    stop: Boolean,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    floating: Boolean = false,
) {
    val c = Scribatic.colors
    Box(
        modifier = modifier
            .size(82.dp)
            .alpha(if (enabled) 1f else 0.38f),
        contentAlignment = Alignment.Center,
    ) {
        if (stop) {
            Box(Modifier.size(82.dp).border(2.dp, c.accent, CircleShape))
        }
        Surface(
            onClick = onClick,
            enabled = enabled,
            shape = CircleShape,
            color = c.accent,
            contentColor = c.onRecord,
            modifier = Modifier
                .size(72.dp)
                .then(if (floating) Modifier.shadow(10.dp, CircleShape) else Modifier)
                .semantics { this.contentDescription = contentDescription; role = Role.Button },
        ) {
            Box(contentAlignment = Alignment.Center) {
                if (stop) {
                    Box(Modifier.size(24.dp).clip(RoundedCornerShape(6.dp)).background(c.onRecord))
                } else {
                    Icon(Icons.mic, contentDescription = null, modifier = Modifier.size(34.dp))
                }
            }
        }
    }
}

/** Pause / Resume beside the record control: a 56dp fill-secondary disc. */
@Composable
fun RoundButton(icon: ImageVector, contentDescription: String, onClick: () -> Unit) {
    val c = Scribatic.colors
    Surface(
        onClick = onClick,
        shape = CircleShape,
        color = c.fillSecondary,
        contentColor = c.ink,
        modifier = Modifier.size(56.dp).semantics { this.contentDescription = contentDescription },
    ) {
        Box(contentAlignment = Alignment.Center) { Icon(icon, contentDescription = null, modifier = Modifier.size(28.dp)) }
    }
}

/** A control with its word underneath: the recorder is where a mistaken tap costs most. */
@Composable
fun LabelledControl(label: String, content: @Composable () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        content()
        Text(label, style = MaterialTheme.typography.labelLarge, color = Scribatic.colors.inkMuted)
    }
}

/** The recorder's phase as a word with a dot. The word is the signal. */
@Composable
fun StatusPill(phase: Phase, label: String) {
    val c = Scribatic.colors
    val (ground, ink, dot) = when (phase) {
        Phase.RECORDING -> Triple(c.accentSoft, c.accentStrong, c.accent)
        Phase.PAUSED -> Triple(c.fillSecondary, c.caution, c.caution)
        Phase.FAILED -> Triple(c.fillSecondary, c.danger, c.danger)
        else -> Triple(c.fillSecondary, c.inkMuted, c.inkSoft)
    }
    Row(
        modifier = Modifier.clip(CircleShape).background(ground).heightIn(min = 30.dp).padding(horizontal = 12.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (phase == Phase.PROCESSING) {
            CircularProgressIndicator(modifier = Modifier.size(12.dp), strokeWidth = 2.dp, color = c.accent, trackColor = c.lineStrong)
        } else {
            val pulse = phase == Phase.RECORDING && !reduceMotion()
            val alpha = if (pulse) {
                val t = rememberInfiniteTransition(label = "pulse")
                val a by t.animateFloat(1f, 0.35f, infiniteRepeatable(tween(600), RepeatMode.Reverse), label = "dot")
                a
            } else 1f
            Box(Modifier.size(8.dp).alpha(alpha).clip(CircleShape).background(dot))
        }
        Text(label, style = MaterialTheme.typography.labelLarge, color = ink)
    }
}

@Composable
private fun reduceMotion(): Boolean {
    val resolver = LocalContext.current.contentResolver
    return Settings.Global.getFloat(resolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
}

/** A small fact: an icon and a short value. */
@Composable
fun MetaChip(icon: ImageVector?, text: String, color: Color = Scribatic.colors.inkMuted, painterRes: Int? = null) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            painterRes != null -> Icon(painterResource(painterRes), contentDescription = null, tint = color, modifier = Modifier.size(16.dp))
            icon != null -> Icon(icon, contentDescription = null, tint = color, modifier = Modifier.size(16.dp))
        }
        Text(text, style = MaterialTheme.typography.labelMedium, color = color)
    }
}

/** A status worth stopping on: "On this phone", "Installed". */
@Composable
fun MetaBadge(icon: ImageVector, text: String) {
    val c = Scribatic.colors
    Row(
        modifier = Modifier.clip(RoundedCornerShape(6.dp)).background(c.fillSecondary).padding(horizontal = 8.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        Icon(icon, contentDescription = null, tint = c.inkMuted, modifier = Modifier.size(14.dp))
        Text(text, style = MaterialTheme.typography.labelLarge, color = c.inkMuted)
    }
}

/** Uppercase Geist Mono label opening a list section. */
@Composable
fun SectionHeader(title: String, trailing: String? = null, modifier: Modifier = Modifier) {
    val c = Scribatic.colors
    Row(
        modifier = modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 22.dp, bottom = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(title.uppercase(), style = MaterialTheme.typography.labelSmall, color = c.inkMuted)
        if (trailing != null) {
            Text(trailing, style = MaterialTheme.typography.labelMedium, color = c.inkSoft)
        }
    }
}

/** Whole-screen message: the brand mark for empty states, a warning for failures. */
@Composable
fun EmptyState(
    title: String,
    body: String,
    failure: Boolean = false,
    aside: String? = null,
    actions: @Composable () -> Unit = {},
) {
    val c = Scribatic.colors
    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 32.dp, vertical = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterVertically),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (failure) {
            Icon(Icons.warning, contentDescription = null, tint = c.danger, modifier = Modifier.size(44.dp))
        } else {
            Image(painterResource(R.drawable.ic_scribatic_mark), contentDescription = null, modifier = Modifier.size(60.dp))
        }
        Text(title, style = MaterialTheme.typography.headlineSmall, color = c.ink, textAlign = TextAlign.Center)
        Text(
            body,
            style = MaterialTheme.typography.bodyMedium,
            color = c.inkMuted,
            textAlign = TextAlign.Center,
            modifier = Modifier.widthIn(max = 320.dp),
        )
        if (aside != null) {
            Text(aside, style = com.scribatic.app.ui.theme.DisplayItalic, color = c.inkSoft)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(top = 8.dp)) { actions() }
    }
}

/** One speaker: dot, name, pencil. Tapping renames. */
@Composable
fun SpeakerChip(name: String, color: Color, onClick: () -> Unit) {
    val c = Scribatic.colors
    Surface(
        onClick = onClick,
        shape = CircleShape,
        color = c.surfaceRaised,
        border = BorderStroke(1.dp, c.lineStrong),
        modifier = Modifier.heightIn(min = 40.dp),
    ) {
        Row(
            modifier = Modifier.padding(start = 12.dp, end = 10.dp, top = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(color))
            Text(name, style = MaterialTheme.typography.labelLarge, color = c.ink)
            Icon(Icons.edit, contentDescription = null, tint = c.inkMuted, modifier = Modifier.size(16.dp))
        }
    }
}
