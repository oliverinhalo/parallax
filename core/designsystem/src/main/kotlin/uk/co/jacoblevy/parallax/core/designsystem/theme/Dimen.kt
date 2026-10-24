package uk.co.jacoblevy.parallax.core.designsystem.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * Spacing scale.
 *
 * A 4dp base with a deliberately sparse set of steps. Having six named sizes rather than every
 * multiple of four is what keeps a large UI visually consistent: there is no "nearly the same but
 * not quite" gap, because no such value exists to reach for.
 */
object Spacing {
    val none: Dp = 0.dp
    val hairline: Dp = 2.dp
    val xs: Dp = 4.dp
    val sm: Dp = 8.dp
    val md: Dp = 12.dp
    val lg: Dp = 16.dp
    val xl: Dp = 24.dp
    val xxl: Dp = 32.dp
    val xxxl: Dp = 48.dp

    /** Horizontal page margin. Generous, as the design direction asks. */
    val screenHorizontal: Dp = 20.dp
    val screenVertical: Dp = 16.dp

    /**
     * Bottom inset for one-handed reach on the capture screen. Controls sit inside this band so
     * the shutter is reachable with a thumb on a large phone.
     */
    val thumbReach: Dp = 96.dp
}

/** The rounded-corner system: 16-24dp, as the design direction specifies. */
object Radii {
    val none: Dp = 0.dp
    val xs: Dp = 6.dp
    val sm: Dp = 10.dp
    val md: Dp = 16.dp
    val lg: Dp = 20.dp
    val xl: Dp = 24.dp
    val xxl: Dp = 32.dp
    val pill: Dp = 999.dp
}

val ParallaxShapes = Shapes(
    extraSmall = RoundedCornerShape(Radii.xs),
    small = RoundedCornerShape(Radii.sm),
    medium = RoundedCornerShape(Radii.md),
    large = RoundedCornerShape(Radii.lg),
    extraLarge = RoundedCornerShape(Radii.xl),
)

/** Elevation steps, matched to the surface ladder in the palette. */
object Elevation {
    val flat: Dp = 0.dp
    val raised: Dp = 2.dp
    val overlay: Dp = 6.dp
    val floating: Dp = 12.dp
    val modal: Dp = 24.dp
}

/** Component sizes that several screens must agree on. */
object Sizing {
    /** Minimum touch target. Android's accessibility guidance floor. */
    val minTouchTarget: Dp = 48.dp
    val iconSmall: Dp = 18.dp
    val iconMedium: Dp = 24.dp
    val iconLarge: Dp = 32.dp

    /** The capture shutter. Large, as the design direction asks for. */
    val shutterOuter: Dp = 76.dp
    val shutterInner: Dp = 62.dp

    val libraryThumbnailMin: Dp = 152.dp
    val modeSwitcherHeight: Dp = 44.dp
    val statPillHeight: Dp = 28.dp
    val progressBarHeight: Dp = 6.dp
    val coverageDomeSize: Dp = 108.dp

    /** While recording, where the coverage map is what the user is steering by. */
    val coverageDomeSizeLarge: Dp = 148.dp

    /** The storage dashboard's stacked bar. Thick enough to read its segments at a glance. */
    val storageBarHeight: Dp = 14.dp
}

/** Blur radii for the glass surfaces on capture overlays. */
object Blur {
    val subtle: Dp = 12.dp
    val standard: Dp = 24.dp
    val heavy: Dp = 40.dp
}
