package com.scribatic.app.ui.theme

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color

/**
 * The Scribatic design-system colour tokens. Same names on iOS
 * (Theme.swift) and the site (--paper, --ink, ...), so a colour change is a
 * one-line edit per platform.
 */
@Immutable
data class ScribaticColors(
    val paper: Color,
    val surfaceRaised: Color,
    val fillSecondary: Color,
    val line: Color,
    val lineStrong: Color,
    val ink: Color,
    val inkMuted: Color,
    val inkSoft: Color,
    /** Brand fill with no small text on it: the record button, the live dot, progress. */
    val accent: Color,
    /** Tangerine that meets text contrast: primary buttons, links, accent text. */
    val accentStrong: Color,
    val accentSoft: Color,
    val onAccent: Color,
    val onRecord: Color,
    val success: Color,
    val caution: Color,
    val danger: Color,
    val speakers: List<Color>,
) {
    /** A per-speaker colour. Never the only signal: the name is always written. */
    fun speaker(index: Int): Color = if (index < 0) inkSoft else speakers[index % speakers.size]
}

val LightScribaticColors = ScribaticColors(
    paper = Color(0xFFF5F5F5),
    surfaceRaised = Color(0xFFFFFFFF),
    fillSecondary = Color(0xFFE6E7EB),
    line = Color(0x1F2D3142),
    lineStrong = Color(0xFF858B9B),
    ink = Color(0xFF2D3142),
    inkMuted = Color(0xFF4F5D75),
    inkSoft = Color(0xFF7A8399),
    accent = Color(0xFFEB6C36),
    accentStrong = Color(0xFFB8491A),
    accentSoft = Color(0xFFFDEEE6),
    onAccent = Color(0xFFFFFFFF),
    onRecord = Color(0xFFFFFFFF),
    success = Color(0xFF2A7D4F),
    caution = Color(0xFF946000),
    danger = Color(0xFFB3263E),
    speakers = listOf(
        Color(0xFFB8491A), Color(0xFF1F5FBF), Color(0xFF2A7D4F), Color(0xFF7B3FB3),
        Color(0xFFB8336A), Color(0xFF147A86), Color(0xFF4B4BC4), Color(0xFF7A5A3F),
    ),
)

val DarkScribaticColors = ScribaticColors(
    paper = Color(0xFF1B1D27),
    surfaceRaised = Color(0xFF252836),
    fillSecondary = Color(0xFF2F3242),
    line = Color(0x24ECEEF3),
    lineStrong = Color(0xFF6B7185),
    ink = Color(0xFFECEEF3),
    inkMuted = Color(0xFFB7BFD4),
    inkSoft = Color(0xFF8B93A8),
    accent = Color(0xFFEB6C36),
    accentStrong = Color(0xFFF0804F),
    accentSoft = Color(0xFF3A2419),
    onAccent = Color(0xFF1B1D27),
    onRecord = Color(0xFFFFFFFF),
    success = Color(0xFF5CC98A),
    caution = Color(0xFFE8B04A),
    danger = Color(0xFFFF6B7F),
    speakers = listOf(
        Color(0xFFF0804F), Color(0xFF7FB0FF), Color(0xFF5CC98A), Color(0xFFC59CF0),
        Color(0xFFF58AB3), Color(0xFF5CC8D6), Color(0xFFA3A3FF), Color(0xFFD2B08F),
    ),
)

val LocalScribaticColors = staticCompositionLocalOf { LightScribaticColors }
