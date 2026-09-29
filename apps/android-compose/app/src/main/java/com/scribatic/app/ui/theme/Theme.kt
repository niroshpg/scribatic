package com.scribatic.app.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.ReadOnlyComposable

object Scribatic {
    val colors: ScribaticColors
        @Composable @ReadOnlyComposable get() = LocalScribaticColors.current
}

/**
 * Dynamic colour is deliberately off: wallpaper-derived colours would replace
 * the brand on Android 12 and later.
 */
@Composable
fun ScribaticTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val c = if (dark) DarkScribaticColors else LightScribaticColors
    val scheme = if (dark) {
        darkColorScheme(
            primary = c.accentStrong, onPrimary = c.onAccent,
            primaryContainer = c.accentSoft, onPrimaryContainer = c.accentStrong,
            secondary = c.ink, onSecondary = c.paper,
            secondaryContainer = c.fillSecondary, onSecondaryContainer = c.ink,
            tertiary = c.accent,
            background = c.paper, onBackground = c.ink,
            surface = c.paper, onSurface = c.ink,
            surfaceVariant = c.fillSecondary, onSurfaceVariant = c.inkMuted,
            surfaceTint = c.paper,
            surfaceContainerLowest = c.surfaceRaised, surfaceContainerLow = c.surfaceRaised,
            surfaceContainer = c.surfaceRaised, surfaceContainerHigh = c.surfaceRaised,
            surfaceContainerHighest = c.fillSecondary,
            outline = c.lineStrong, outlineVariant = c.line,
            error = c.danger, onError = c.paper,
        )
    } else {
        lightColorScheme(
            primary = c.accentStrong, onPrimary = c.onAccent,
            primaryContainer = c.accentSoft, onPrimaryContainer = c.accentStrong,
            secondary = c.ink, onSecondary = c.paper,
            secondaryContainer = c.fillSecondary, onSecondaryContainer = c.ink,
            tertiary = c.accent,
            background = c.paper, onBackground = c.ink,
            surface = c.paper, onSurface = c.ink,
            surfaceVariant = c.fillSecondary, onSurfaceVariant = c.inkMuted,
            surfaceTint = c.paper,
            surfaceContainerLowest = c.surfaceRaised, surfaceContainerLow = c.surfaceRaised,
            surfaceContainer = c.surfaceRaised, surfaceContainerHigh = c.surfaceRaised,
            surfaceContainerHighest = c.fillSecondary,
            outline = c.lineStrong, outlineVariant = c.line,
            error = c.danger, onError = c.surfaceRaised,
        )
    }
    CompositionLocalProvider(LocalScribaticColors provides c) {
        MaterialTheme(colorScheme = scheme, typography = ScribaticTypography, content = content)
    }
}
