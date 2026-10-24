package uk.co.jacoblevy.parallax.core.designsystem.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.draw.clip
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import uk.co.jacoblevy.parallax.core.common.result.ParallaxError
import uk.co.jacoblevy.parallax.core.common.result.RecoveryAction
import uk.co.jacoblevy.parallax.core.common.util.formatBytes
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing

/** An empty state: an icon, a title, an explanation and at most one action. */
@Composable
fun EmptyState(
    title: String,
    message: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
) {
    Column(
        modifier
            .fillMaxSize()
            .padding(Spacing.xl),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = ParallaxTheme.colors.textTertiary,
                modifier = Modifier.size(56.dp),
            )
            Spacer(Modifier.height(Spacing.lg))
        }
        Text(
            text = title,
            style = MaterialTheme.typography.headlineSmall,
            color = ParallaxTheme.colors.textPrimary,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(Spacing.sm))
        Text(
            text = message,
            style = MaterialTheme.typography.bodyMedium,
            color = ParallaxTheme.colors.textSecondary,
            textAlign = TextAlign.Center,
        )
        if (actionLabel != null && onAction != null) {
            Spacer(Modifier.height(Spacing.xl))
            ParallaxButton(text = actionLabel, onClick = onAction)
        }
    }
}

/**
 * An error state driven by a [ParallaxError].
 *
 * The button comes from the error's own [RecoveryAction], so there is no way to show an error
 * without offering the action that resolves it. That is the whole reason [ParallaxError] carries
 * a recovery action rather than just a message.
 */
@Composable
fun ErrorState(
    error: ParallaxError,
    modifier: Modifier = Modifier,
    onRecover: (RecoveryAction) -> Unit = {},
    icon: ImageVector? = null,
) {
    val recovery = error.recovery
    val label = recoveryLabel(recovery)
    Column(
        modifier
            .fillMaxSize()
            .padding(Spacing.xl),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = ParallaxTheme.colors.warning,
                modifier = Modifier.size(48.dp),
            )
            Spacer(Modifier.height(Spacing.lg))
        }
        Text(
            text = error.message,
            style = MaterialTheme.typography.titleMedium,
            color = ParallaxTheme.colors.textPrimary,
            textAlign = TextAlign.Center,
        )
        val detail = recoveryDetail(error)
        if (detail != null) {
            Spacer(Modifier.height(Spacing.sm))
            Text(
                text = detail,
                style = MaterialTheme.typography.bodyMedium,
                color = ParallaxTheme.colors.textSecondary,
                textAlign = TextAlign.Center,
            )
        }
        if (label != null) {
            Spacer(Modifier.height(Spacing.xl))
            ParallaxButton(text = label, onClick = { onRecover(recovery) })
        }
    }
}

/** The button label for a recovery action, or null when there is nothing to offer. */
private fun recoveryLabel(action: RecoveryAction): String? = when (action) {
    RecoveryAction.None -> null
    RecoveryAction.Retry -> "Try again"
    RecoveryAction.WaitAndRetry -> "Try again"
    RecoveryAction.Recapture -> "Scan again"
    RecoveryAction.GoToLibrary -> "Back to library"
    is RecoveryAction.FreeStorage -> "Manage storage"
    is RecoveryAction.ResumeFrom -> "Resume processing"
    is RecoveryAction.ProcessPartial -> "Process what we have"
    is RecoveryAction.GrantPermission -> "Grant permission"
}

/** The extra sentence that makes an error actionable rather than merely descriptive. */
private fun recoveryDetail(error: ParallaxError): String? = when (val recovery = error.recovery) {
    is RecoveryAction.FreeStorage ->
        "Free up about ${formatBytes(recovery.bytesNeeded)} and try again."
    is RecoveryAction.ResumeFrom ->
        "Everything up to ${recovery.stage} was saved, so processing can pick up from there."
    is RecoveryAction.ProcessPartial ->
        "${recovery.usableFrames} frames are usable. The model will cover less of the subject."
    RecoveryAction.WaitAndRetry ->
        "Leave the phone to cool for a minute or two first."
    else -> when (error) {
        is ParallaxError.CaptureTooShort -> error.message
        is ParallaxError.DeviceUnsupported -> error.message
        else -> null
    }
}

/** A full-screen loading state with an optional message. */
@Composable
fun LoadingState(modifier: Modifier = Modifier, message: String? = null) {
    Column(
        modifier.fillMaxSize(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        androidx.compose.material3.CircularProgressIndicator(
            color = ParallaxTheme.colors.accent,
            strokeWidth = 3.dp,
            modifier = Modifier.size(36.dp),
        )
        if (message != null) {
            Spacer(Modifier.height(Spacing.lg))
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
                color = ParallaxTheme.colors.textSecondary,
            )
        }
    }
}

/** A shimmering placeholder block, for library thumbnails that are still rendering. */
@Composable
fun SkeletonBlock(modifier: Modifier = Modifier) {
    val transition = rememberInfiniteTransition(label = "skeleton")
    // A slow, shallow pulse. A fast or high-contrast shimmer draws the eye to the thing that is
    // not ready yet, which is the opposite of what a placeholder should do.
    val alpha by transition.animateFloat(
        initialValue = 0.30f,
        targetValue = 0.55f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 900, easing = LinearEasing),
            repeatMode = RepeatMode.Reverse,
        ),
        label = "skeletonAlpha",
    )
    Box(
        modifier
            .clip(RoundedCornerShape(Radii.md))
            .background(ParallaxTheme.colors.surfaceOverlay.copy(alpha = alpha)),
    )
}
