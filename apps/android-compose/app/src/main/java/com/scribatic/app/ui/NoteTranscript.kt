package com.scribatic.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.scribatic.app.engine.SpeakerLabel
import com.scribatic.app.engine.TranscriptSegment
import com.scribatic.app.ui.theme.Scribatic

/** Consecutive segments shown as one card: one speaker's turn, or one paragraph of a lecture. */
data class TranscriptBlock(val speaker: Int, val segments: List<TranscriptSegment>) {
    val startMs: Long get() = segments.first().startMs
    val segmentIds: List<Long> get() = segments.map { it.id }
    val key: Long get() = segments.first().id.takeIf { it != 0L } ?: startMs
}

/** A discussion: a new card wherever the speaker changes. */
fun discussionBlocks(segments: List<TranscriptSegment>): List<TranscriptBlock> =
    segments.fold(mutableListOf()) { blocks, segment ->
        val last = blocks.lastOrNull()
        if (last != null && last.speaker == segment.speaker) {
            blocks[blocks.lastIndex] = last.copy(segments = last.segments + segment)
        } else {
            blocks += TranscriptBlock(segment.speaker, listOf(segment))
        }
        blocks
    }

/**
 * A lecture: one voice, so cards are paragraphs — a new one after a pause, or
 * once a paragraph runs long enough to be hard to read as one.
 */
fun lectureBlocks(segments: List<TranscriptSegment>, pauseMs: Long = 2500, maxPerBlock: Int = 6): List<TranscriptBlock> =
    segments.fold(mutableListOf()) { blocks, segment ->
        val last = blocks.lastOrNull()
        val continues = last != null && last.segments.size < maxPerBlock &&
            segment.startMs - last.segments.last().endMs < pauseMs
        if (continues) {
            blocks[blocks.lastIndex] = last!!.copy(segments = last.segments + segment)
        } else {
            blocks += TranscriptBlock(segment.speaker, listOf(segment))
        }
        blocks
    }

/** "S1" until the speaker is named, then their initials: "AN" for "Ana", "PS" for "Priya Shah". */
fun speakerInitials(label: SpeakerLabel?, index: Int): String {
    val name = label?.name?.trim().orEmpty()
    if (name.isEmpty()) return "S${index + 1}"
    val words = name.split(Regex("\\s+")).filter { it.isNotEmpty() }
    return if (words.size >= 2) "${words[0].first()}${words[1].first()}".uppercase()
    else words[0].take(2).uppercase()
}

@Composable
fun SpeakerAvatar(initials: String, index: Int, modifier: Modifier = Modifier) {
    val c = Scribatic.colors
    val color = c.speaker(index)
    Box(
        modifier = modifier.size(36.dp).clip(CircleShape).background(color.copy(alpha = 0.18f)),
        contentAlignment = Alignment.Center,
    ) {
        Text(initials, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold, color = color)
    }
}

/**
 * One speaker's turn: their avatar beside a card, in the manner of a chat. The
 * title is the name the user gave; until then only the avatar says who it is.
 * Tapping either opens [onSpeaker], to rename, move the turn or split it off.
 */
@Composable
fun DiscussionCard(block: TranscriptBlock, label: SpeakerLabel?, onSpeaker: () -> Unit) {
    val c = Scribatic.colors
    val identified = block.speaker >= 0
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 5.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.Top,
    ) {
        if (identified) {
            SpeakerAvatar(
                speakerInitials(label, block.speaker),
                block.speaker,
                Modifier.clickable(onClickLabel = "Change speaker", role = Role.Button, onClick = onSpeaker),
            )
        }
        Column(
            Modifier
                .weight(1f)
                .clip(RoundedCornerShape(topStart = 4.dp, topEnd = 16.dp, bottomEnd = 16.dp, bottomStart = 16.dp))
                .background(c.surfaceRaised)
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            val name = label?.name?.trim().orEmpty()
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (identified && name.isNotEmpty()) {
                    Text(
                        name,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.SemiBold,
                        color = c.speaker(block.speaker),
                        modifier = Modifier.weight(1f).clickable(onClickLabel = "Change speaker", onClick = onSpeaker),
                    )
                } else {
                    Box(Modifier.weight(1f))
                }
                Text(clock(block.startMs), style = MaterialTheme.typography.labelMedium, color = c.inkSoft)
            }
            block.segments.forEach { Text(it.text, style = MaterialTheme.typography.bodyLarge, color = c.ink) }
        }
    }
}

/** A paragraph of a lecture: no avatar and no title, one voice throughout. */
@Composable
fun LectureCard(block: TranscriptBlock) {
    val c = Scribatic.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 5.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(c.surfaceRaised)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text(clock(block.startMs), style = MaterialTheme.typography.labelMedium, color = c.inkSoft)
        Text(
            block.segments.joinToString(" ") { it.text },
            style = MaterialTheme.typography.bodyLarge,
            color = c.ink,
        )
    }
}
