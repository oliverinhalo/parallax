package uk.co.jacoblevy.parallax.core.designsystem.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.LocalContext

/**
 * Semantic colours, reachable as `ParallaxTheme.colors`.
 *
 * Material 3's ColorScheme has no slot for a coverage ramp, a camera scrim or a glass surface,
 * and bending `tertiary` into one of those makes every call site a guess. These live alongside
 * the ColorScheme instead, so Material components still theme correctly while Parallax's own
 * components name what they actually mean.
 */
val LocalParallaxColors = staticCompositionLocalOf { DarkParallaxColors }

object ParallaxTheme {
    val colors: ParallaxColors
        @Composable @ReadOnlyComposable get() = LocalParallaxColors.current
}

private val DarkColorScheme = darkColorScheme(
    primary = ParallaxPalette.Cyan,
    onPrimary = ParallaxPalette.CyanInk,
    primaryContainer = ParallaxPalette.CyanDim,
    onPrimaryContainer = ParallaxPalette.CyanInk,
    inversePrimary = ParallaxPalette.CyanOnLight,

    secondary = ParallaxPalette.Amber,
    onSecondary = ParallaxPalette.AmberInk,
    secondaryContainer = ParallaxPalette.AmberDim,
    onSecondaryContainer = ParallaxPalette.AmberInk,

    tertiary = ParallaxPalette.Mint,
    onTertiary = ParallaxPalette.MintInk,

    background = ParallaxPalette.Graphite0,
    onBackground = ParallaxPalette.InkHigh,
    surface = ParallaxPalette.Graphite1,
    onSurface = ParallaxPalette.InkHigh,
    surfaceVariant = ParallaxPalette.Graphite3,
    onSurfaceVariant = ParallaxPalette.InkMedium,
    surfaceContainerLowest = ParallaxPalette.Graphite0,
    surfaceContainerLow = ParallaxPalette.Graphite1,
    surfaceContainer = ParallaxPalette.Graphite2,
    surfaceContainerHigh = ParallaxPalette.Graphite3,
    surfaceContainerHighest = ParallaxPalette.Graphite4,
    inverseSurface = ParallaxPalette.Paper1,
    inverseOnSurface = ParallaxPalette.SlateHigh,

    error = ParallaxPalette.Rose,
    onError = ParallaxPalette.RoseInk,
    errorContainer = ParallaxPalette.RoseDim,
    onErrorContainer = ParallaxPalette.RoseInk,

    outline = ParallaxPalette.OutlineDark,
    outlineVariant = ParallaxPalette.OutlineDarkSubtle,
    scrim = ParallaxPalette.Graphite0,
)

private val LightColorScheme = lightColorScheme(
    primary = ParallaxPalette.CyanOnLight,
    onPrimary = ParallaxPalette.Paper0,
    primaryContainer = ParallaxPalette.CyanBright,
    onPrimaryContainer = ParallaxPalette.CyanInk,
    inversePrimary = ParallaxPalette.Cyan,

    secondary = ParallaxPalette.AmberOnLight,
    onSecondary = ParallaxPalette.Paper0,
    secondaryContainer = ParallaxPalette.AmberBright,
    onSecondaryContainer = ParallaxPalette.AmberInk,

    tertiary = ParallaxPalette.MintOnLight,
    onTertiary = ParallaxPalette.Paper0,

    background = ParallaxPalette.Paper1,
    onBackground = ParallaxPalette.SlateHigh,
    surface = ParallaxPalette.Paper0,
    onSurface = ParallaxPalette.SlateHigh,
    surfaceVariant = ParallaxPalette.Paper2,
    onSurfaceVariant = ParallaxPalette.SlateMedium,
    surfaceContainerLowest = ParallaxPalette.Paper0,
    surfaceContainerLow = ParallaxPalette.Paper1,
    surfaceContainer = ParallaxPalette.Paper2,
    surfaceContainerHigh = ParallaxPalette.Paper3,
    surfaceContainerHighest = ParallaxPalette.Paper4,
    inverseSurface = ParallaxPalette.Graphite2,
    inverseOnSurface = ParallaxPalette.InkHigh,

    error = ParallaxPalette.RoseOnLight,
    onError = ParallaxPalette.Paper0,
    errorContainer = ParallaxPalette.RoseContainerLight,
    onErrorContainer = ParallaxPalette.RoseInk,

    outline = ParallaxPalette.OutlineLight,
    outlineVariant = ParallaxPalette.OutlineLightSubtle,
    scrim = ParallaxPalette.SlateHigh,
)

/**
 * @param darkTheme whether to use the dark scheme
 * @param dynamicColor opt in to Material You. Off by default: the brand palette is the app's
 *   identity, and a scanner's coverage and progress visuals rely on the cyan reading as "live",
 *   which a wallpaper-derived hue cannot guarantee.
 */
@Composable
fun ParallaxTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val colorScheme = when {
        // Dynamic colour needs API 31; below that the flag is simply ignored.
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        darkTheme -> DarkColorScheme
        else -> LightColorScheme
    }
    // The semantic colours keep the brand accent even under dynamic colour, because the
    // coverage ramp and the camera scrim are functional rather than decorative.
    val parallaxColors = if (darkTheme) DarkParallaxColors else LightParallaxColors

    CompositionLocalProvider(LocalParallaxColors provides parallaxColors) {
        MaterialTheme(
            colorScheme = colorScheme,
            typography = ParallaxTypography,
            shapes = ParallaxShapes,
        ) {
            // The theme paints its own background rather than leaving it to
            // the window. The activity's `windowBackground` follows the
            // *system* dark mode, while this follows the user's choice in
            // settings, so a screen without its own surface -- the viewer, the
            // camera overlays -- would otherwise be drawn on a light window
            // whenever the two disagree.
            Surface(color = parallaxColors.surfaceSunken, content = content)
        }
    }
}
