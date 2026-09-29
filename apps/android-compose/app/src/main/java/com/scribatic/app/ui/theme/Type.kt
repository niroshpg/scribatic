package com.scribatic.app.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.scribatic.app.R

/** Titles only. Never body copy or buttons. */
val InstrumentSerif = FontFamily(
    Font(R.font.instrument_serif_regular, FontWeight.Normal),
    Font(R.font.instrument_serif_italic, FontWeight.Normal, FontStyle.Italic),
)

/** All interface text. */
val Geist = FontFamily(
    Font(R.font.geist_regular, FontWeight.Normal),
    Font(R.font.geist_medium, FontWeight.Medium),
    Font(R.font.geist_semibold, FontWeight.SemiBold),
)

/** Numbers that change or line up, and uppercase section labels. */
val GeistMono = FontFamily(
    Font(R.font.geist_mono_regular, FontWeight.Normal),
    Font(R.font.geist_mono_medium, FontWeight.Medium),
)

private val tabular = "tnum"

/**
 * Material slots mapped to the design-system type scale:
 * headlineMedium = display-lg (large app bar), headlineSmall = display-md,
 * titleLarge = display-sm, titleMedium = title, bodyLarge = body,
 * bodyMedium = body-sm, bodySmall = caption, labelLarge = label,
 * labelMedium = timestamp, labelSmall = eyebrow.
 */
val ScribaticTypography = Typography(
    headlineLarge = TextStyle(fontFamily = InstrumentSerif, fontSize = 34.sp, lineHeight = 40.sp),
    headlineMedium = TextStyle(fontFamily = InstrumentSerif, fontSize = 34.sp, lineHeight = 40.sp),
    headlineSmall = TextStyle(fontFamily = InstrumentSerif, fontSize = 26.sp, lineHeight = 32.sp),
    titleLarge = TextStyle(fontFamily = InstrumentSerif, fontSize = 22.sp, lineHeight = 28.sp),
    titleMedium = TextStyle(fontFamily = Geist, fontWeight = FontWeight.SemiBold, fontSize = 17.sp, lineHeight = 24.sp),
    titleSmall = TextStyle(fontFamily = Geist, fontWeight = FontWeight.Medium, fontSize = 15.sp, lineHeight = 22.sp),
    bodyLarge = TextStyle(fontFamily = Geist, fontSize = 16.sp, lineHeight = 25.sp),
    bodyMedium = TextStyle(fontFamily = Geist, fontSize = 15.sp, lineHeight = 22.sp),
    bodySmall = TextStyle(fontFamily = Geist, fontSize = 13.sp, lineHeight = 18.sp),
    labelLarge = TextStyle(fontFamily = Geist, fontWeight = FontWeight.Medium, fontSize = 14.sp, lineHeight = 20.sp),
    labelMedium = TextStyle(fontFamily = GeistMono, fontSize = 12.5.sp, lineHeight = 16.sp, fontFeatureSettings = tabular),
    labelSmall = TextStyle(
        fontFamily = GeistMono, fontWeight = FontWeight.Medium, fontSize = 11.5.sp, lineHeight = 16.sp,
        letterSpacing = 0.14.em,
    ),
)

/** The small italic aside: one per screen at most. */
val DisplayItalic = TextStyle(fontFamily = InstrumentSerif, fontStyle = FontStyle.Italic, fontSize = 18.sp, lineHeight = 26.sp)
