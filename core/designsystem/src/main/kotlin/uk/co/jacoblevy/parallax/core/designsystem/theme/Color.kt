package uk.co.jacoblevy.parallax.core.designsystem.theme

import androidx.compose.ui.graphics.Color

/**
 * The Parallax palette.
 *
 * Dark-first. Surfaces are near-black graphite in layered elevation steps rather than the
 * translucent overlays Material 3 defaults to: a capture viewfinder sits behind most of this UI,
 * and translucent surfaces over live camera are unreadable.
 *
 * One signature accent, an electric cyan, is used *only* for active state, coverage and progress.
 * Keeping it scarce is what makes it read as "this is live" rather than as decoration. A warm
 * amber carries warnings and guidance so the two never compete.
 */
internal object ParallaxPalette {
    // ---- brand ------------------------------------------------------------
    val Cyan = Color(0xFF3DE0FF)
    val CyanBright = Color(0xFF7FEBFF)
    val CyanDim = Color(0xFF1C9FBB)
    val CyanInk = Color(0xFF002630)

    val Amber = Color(0xFFFFB454)
    val AmberBright = Color(0xFFFFCC8A)
    val AmberDim = Color(0xFFB87A2A)
    val AmberInk = Color(0xFF2B1A00)

    val Rose = Color(0xFFFF6B7A)
    val RoseDim = Color(0xFFB83E4A)
    val RoseInk = Color(0xFF330A0E)

    val Mint = Color(0xFF5FE3A1)
    val MintInk = Color(0xFF00301C)

    // ---- dark surfaces: a measured elevation ladder, not alpha overlays ---
    val Graphite0 = Color(0xFF0A0B0D)
    val Graphite1 = Color(0xFF121417)
    val Graphite2 = Color(0xFF1A1D21)
    val Graphite3 = Color(0xFF23272C)
    val Graphite4 = Color(0xFF2E3338)
    val Graphite5 = Color(0xFF3A4046)

    val InkHigh = Color(0xFFF2F5F7)
    val InkMedium = Color(0xFFB4BCC4)
    val InkLow = Color(0xFF7A838C)
    val InkDisabled = Color(0xFF4A5157)

    val OutlineDark = Color(0xFF353B41)
    val OutlineDarkSubtle = Color(0xFF23272C)

    // ---- light surfaces ---------------------------------------------------
    val Paper0 = Color(0xFFFFFFFF)
    val Paper1 = Color(0xFFF7F8FA)
    val Paper2 = Color(0xFFEFF1F4)
    val Paper3 = Color(0xFFE4E7EB)
    val Paper4 = Color(0xFFD7DBE0)

    val SlateHigh = Color(0xFF14171A)
    val SlateMedium = Color(0xFF3F464D)
    val SlateLow = Color(0xFF6B737B)
    val SlateDisabled = Color(0xFFA4ACB4)

    val OutlineLight = Color(0xFFC9CED4)
    val OutlineLightSubtle = Color(0xFFE4E7EB)

    // Light-theme accents are darkened so they still pass contrast on white.
    val CyanOnLight = Color(0xFF007A96)
    val RoseContainerLight = Color(0xFFFFDAD9)
    val AmberOnLight = Color(0xFF8A5600)
    val RoseOnLight = Color(0xFFB3122A)
    val MintOnLight = Color(0xFF00674A)
}

/**
 * Semantic colours that Material 3's ColorScheme has no slot for.
 *
 * Coverage and quality visuals must be colour-blind safe, so they are built from a
 * cyan-to-amber ramp that separates by lightness as well as hue; a red-green ramp would be the
 * obvious choice and would be unreadable for the most common form of colour blindness.
 */
data class ParallaxColors(
    val surfaceSunken: Color,
    val surfaceRaised: Color,
    val surfaceOverlay: Color,
    val surfaceGlass: Color,
    val accent: Color,
    val accentMuted: Color,
    val onAccent: Color,
    val warning: Color,
    val onWarning: Color,
    val danger: Color,
    val onDanger: Color,
    val success: Color,
    val onSuccess: Color,
    val textPrimary: Color,
    val textSecondary: Color,
    val textTertiary: Color,
    val textDisabled: Color,
    val outline: Color,
    val outlineSubtle: Color,
    /** Low coverage, through to full coverage. Ordered; interpolate between them. */
    val coverageRamp: List<Color>,
    /** Progress track and fill for the processing visual. */
    val progressTrack: Color,
    val progressFill: Color,
    /** The scrim behind capture controls over a live camera feed. */
    val cameraScrim: Color,
    val isDark: Boolean,
) {
    /** Colour for a coverage value in 0..1, interpolated along [coverageRamp]. */
    fun coverageColor(fraction: Float): Color {
        if (coverageRamp.isEmpty()) return accent
        if (coverageRamp.size == 1) return coverageRamp.first()
        val clamped = fraction.coerceIn(0f, 1f)
        val scaled = clamped * (coverageRamp.size - 1)
        val index = scaled.toInt().coerceAtMost(coverageRamp.size - 2)
        val t = scaled - index
        return androidx.compose.ui.graphics.lerp(coverageRamp[index], coverageRamp[index + 1], t)
    }
}

internal val DarkParallaxColors = ParallaxColors(
    surfaceSunken = ParallaxPalette.Graphite0,
    surfaceRaised = ParallaxPalette.Graphite2,
    surfaceOverlay = ParallaxPalette.Graphite3,
    surfaceGlass = Color(0xCC16191D),
    accent = ParallaxPalette.Cyan,
    accentMuted = ParallaxPalette.CyanDim,
    onAccent = ParallaxPalette.CyanInk,
    warning = ParallaxPalette.Amber,
    onWarning = ParallaxPalette.AmberInk,
    danger = ParallaxPalette.Rose,
    onDanger = ParallaxPalette.RoseInk,
    success = ParallaxPalette.Mint,
    onSuccess = ParallaxPalette.MintInk,
    textPrimary = ParallaxPalette.InkHigh,
    textSecondary = ParallaxPalette.InkMedium,
    textTertiary = ParallaxPalette.InkLow,
    textDisabled = ParallaxPalette.InkDisabled,
    outline = ParallaxPalette.OutlineDark,
    outlineSubtle = ParallaxPalette.OutlineDarkSubtle,
    coverageRamp = listOf(
        Color(0xFF2A2F34),
        ParallaxPalette.CyanDim,
        ParallaxPalette.Cyan,
        ParallaxPalette.CyanBright,
    ),
    progressTrack = ParallaxPalette.Graphite4,
    progressFill = ParallaxPalette.Cyan,
    cameraScrim = Color(0x99000000),
    isDark = true,
)

internal val LightParallaxColors = ParallaxColors(
    surfaceSunken = ParallaxPalette.Paper2,
    surfaceRaised = ParallaxPalette.Paper0,
    surfaceOverlay = ParallaxPalette.Paper1,
    surfaceGlass = Color(0xCCFFFFFF),
    accent = ParallaxPalette.CyanOnLight,
    accentMuted = Color(0xFF66B8CC),
    onAccent = Color(0xFFFFFFFF),
    warning = ParallaxPalette.AmberOnLight,
    onWarning = Color(0xFFFFFFFF),
    danger = ParallaxPalette.RoseOnLight,
    onDanger = Color(0xFFFFFFFF),
    success = ParallaxPalette.MintOnLight,
    onSuccess = Color(0xFFFFFFFF),
    textPrimary = ParallaxPalette.SlateHigh,
    textSecondary = ParallaxPalette.SlateMedium,
    textTertiary = ParallaxPalette.SlateLow,
    textDisabled = ParallaxPalette.SlateDisabled,
    outline = ParallaxPalette.OutlineLight,
    outlineSubtle = ParallaxPalette.OutlineLightSubtle,
    coverageRamp = listOf(
        Color(0xFFDCE1E6),
        Color(0xFF8FCEDD),
        ParallaxPalette.CyanOnLight,
        Color(0xFF00546B),
    ),
    progressTrack = ParallaxPalette.Paper3,
    progressFill = ParallaxPalette.CyanOnLight,
    // Capture is always rendered over live camera, so the scrim stays dark even
    // in the light theme; a light scrim over a bright scene is illegible.
    cameraScrim = Color(0x99000000),
    isDark = false,
)
