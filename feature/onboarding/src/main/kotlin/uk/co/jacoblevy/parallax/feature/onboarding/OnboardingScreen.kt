package uk.co.jacoblevy.parallax.feature.onboarding

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxButton
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxCard
import uk.co.jacoblevy.parallax.core.designsystem.component.ParallaxTextButton
import uk.co.jacoblevy.parallax.core.designsystem.icon.ParallaxIcons
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.core.designsystem.theme.Radii
import uk.co.jacoblevy.parallax.core.designsystem.theme.Spacing
import uk.co.jacoblevy.parallax.core.model.CaptureMode

@Composable
fun OnboardingRoute(
    onFinished: () -> Unit,
    viewModel: OnboardingViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    LaunchedEffect(state.finished) { if (state.finished) onFinished() }
    OnboardingScreen(state = state, onEvent = viewModel::onEvent)
}

@Composable
internal fun OnboardingScreen(
    state: OnboardingUiState,
    onEvent: (OnboardingEvent) -> Unit,
) {
    val context = LocalContext.current
    val activity = context as? Activity
    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission(),
    ) { granted ->
        // After a denial, `shouldShowRequestPermissionRationale` is the only way to tell
        // "they said no this time" from "Android will never ask again". Without it the screen
        // would keep offering a button that silently does nothing.
        val canAskAgain = granted || activity?.let {
            ActivityCompat.shouldShowRequestPermissionRationale(it, Manifest.permission.CAMERA)
        } ?: true
        onEvent(OnboardingEvent.PermissionResult(granted = granted, canAskAgain = canAskAgain))
    }

    // Permission can also be granted outside the app, in system settings, so the state is
    // re-read rather than being remembered from the launcher result alone.
    LaunchedEffect(Unit) {
        val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) onEvent(OnboardingEvent.PermissionResult(granted = true, canAskAgain = true))
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(ParallaxTheme.colors.surfaceSunken)
            .padding(horizontal = Spacing.screenHorizontal),
    ) {
        Column(Modifier.fillMaxSize()) {
            Spacer(Modifier.height(Spacing.xxl))
            PageIndicator(state)
            AnimatedContent(
                targetState = state.page,
                transitionSpec = {
                    val forward = targetState.ordinal > initialState.ordinal
                    val offset = if (forward) 1 else -1
                    (
                        slideInHorizontally(tween(PAGE_MILLIS)) { width -> offset * width / 4 } +
                            fadeIn(tween(PAGE_MILLIS))
                        ) togetherWith (
                        slideOutHorizontally(tween(PAGE_MILLIS)) { width -> -offset * width / 4 } +
                            fadeOut(tween(PAGE_MILLIS))
                        )
                },
                modifier = Modifier.weight(1f),
                label = "onboardingPage",
            ) { page ->
                when (page) {
                    OnboardingPage.Welcome -> WelcomePage()
                    OnboardingPage.Modes -> ModesPage(state)
                    OnboardingPage.Technique -> TechniquePage()
                    OnboardingPage.Permission -> PermissionPage(
                        state = state,
                        onRequest = { permissionLauncher.launch(Manifest.permission.CAMERA) },
                        onOpenAppSettings = { context.startActivity(appSettingsIntent(context)) },
                    )
                }
            }
            Footer(state, onEvent)
            Spacer(Modifier.height(Spacing.xl))
        }
    }
}

@Composable
private fun PageIndicator(state: OnboardingUiState) {
    Row(
        Modifier
            .fillMaxWidth()
            .semantics {
                contentDescription = "Step ${state.pageIndex + 1} of ${state.pageCount}"
            },
        horizontalArrangement = Arrangement.spacedBy(Spacing.xs),
    ) {
        repeat(state.pageCount) { index ->
            val active = index <= state.pageIndex
            Box(
                Modifier
                    .weight(1f)
                    .height(INDICATOR_HEIGHT.dp)
                    .clip(RoundedCornerShape(Radii.pill))
                    .background(
                        if (active) {
                            ParallaxTheme.colors.accent
                        } else {
                            ParallaxTheme.colors.surfaceOverlay
                        },
                    ),
            )
        }
    }
}

@Composable
private fun WelcomePage() {
    PageBody(
        icon = ParallaxIcons.ModeObject,
        title = "Capture the world in depth",
        body = "Point your phone at an object, a room or an open space and walk around it. " +
            "Parallax turns what the camera sees into a measurable 3D model.",
    ) {
        ParallaxCard {
            Column {
                Claim(
                    icon = ParallaxIcons.Info,
                    title = "Nothing leaves this device",
                    body = "Scanning, reconstruction and storage all happen here. Parallax " +
                        "makes no network requests and works in airplane mode.",
                )
                Spacer(Modifier.height(Spacing.md))
                Claim(
                    icon = ParallaxIcons.Measure,
                    title = "Real measurements",
                    body = "Scans are metric, so distances, heights and floor areas you " +
                        "measure in the viewer are real.",
                )
            }
        }
    }
}

@Composable
private fun ModesPage(state: OnboardingUiState) {
    PageBody(
        icon = ParallaxIcons.Scan,
        title = "Four ways to scan",
        body = "Pick the mode that matches what is in front of you.",
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(Spacing.sm)) {
            ModeRow(
                icon = ParallaxIcons.ModeObject,
                title = "Object",
                body = "Orbit something small. The background is removed automatically.",
                available = CaptureMode.Object in state.capabilities.availableCaptureModes,
            )
            ModeRow(
                icon = ParallaxIcons.ModeRoom,
                title = "Room",
                body = "Walk an interior. You also get a measured floor plan.",
                available = CaptureMode.Room in state.capabilities.availableCaptureModes,
            )
            ModeRow(
                icon = ParallaxIcons.ModeArea,
                title = "Area",
                body = "Larger outdoor spaces, captured in chunks.",
                available = CaptureMode.Area in state.capabilities.availableCaptureModes,
            )
            ModeRow(
                icon = ParallaxIcons.ModeImport,
                title = "Import",
                body = "Build a model from photos or video you already have.",
                available = true,
            )
        }
    }
}

@Composable
private fun TechniquePage() {
    PageBody(
        icon = ParallaxIcons.Walk,
        title = "Move slowly, cover everything",
        body = "How you move matters more than your phone's camera.",
    ) {
        ParallaxCard {
            Column(verticalArrangement = Arrangement.spacedBy(Spacing.md)) {
                Claim(
                    icon = ParallaxIcons.ChevronRight,
                    title = "Walk, don't pan",
                    body = "Depth comes from seeing the same surface from different " +
                        "positions. Standing still and rotating gives the maths nothing " +
                        "to work with.",
                )
                Claim(
                    icon = ParallaxIcons.ChevronUp,
                    title = "Three heights",
                    body = "One pass at eye level, one lower, one from above. The overlay " +
                        "shows what is still missing.",
                )
                Claim(
                    icon = ParallaxIcons.Lighting,
                    title = "Even light",
                    body = "Avoid shooting into a window. Hard shadows get baked into the " +
                        "texture.",
                )
            }
        }
    }
}

@Composable
private fun PermissionPage(
    state: OnboardingUiState,
    onRequest: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    PageBody(
        icon = ParallaxIcons.Scan,
        title = if (state.cameraGranted) "You're ready" else "Camera access",
        body = when {
            state.cameraGranted ->
                "That's everything. Your first scan is the hardest; the on-screen guidance " +
                    "will tell you when to slow down or move."
            state.permissionPermanentlyDenied ->
                "Android will no longer ask for you. Grant camera access in the system " +
                    "settings for Parallax, then come back."
            else ->
                "Parallax needs the camera to scan. It is used only while you are capturing, " +
                    "and the frames stay on this device."
        },
    ) {
        when {
            state.cameraGranted -> Unit
            state.permissionPermanentlyDenied ->
                ParallaxButton(
                    text = "Open app settings",
                    onClick = onOpenAppSettings,
                    icon = ParallaxIcons.Settings,
                )
            else ->
                ParallaxButton(text = "Allow camera", onClick = onRequest, icon = ParallaxIcons.Scan)
        }
    }
}

/** The shared page layout: icon, heading, paragraph, then page-specific content. */
@Composable
private fun PageBody(
    icon: ImageVector,
    title: String,
    body: String,
    content: @Composable () -> Unit,
) {
    Column(
        Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.Center,
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = ParallaxTheme.colors.accent,
            modifier = Modifier.size(HERO_ICON.dp),
        )
        Spacer(Modifier.height(Spacing.xl))
        Text(
            text = title,
            style = MaterialTheme.typography.headlineMedium,
            color = ParallaxTheme.colors.textPrimary,
        )
        Spacer(Modifier.height(Spacing.sm))
        Text(
            text = body,
            style = MaterialTheme.typography.bodyLarge,
            color = ParallaxTheme.colors.textSecondary,
        )
        Spacer(Modifier.height(Spacing.xl))
        content()
    }
}

@Composable
private fun Claim(icon: ImageVector, title: String, body: String) {
    Row(Modifier.semantics(mergeDescendants = true) { contentDescription = "$title. $body" }) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = ParallaxTheme.colors.accent,
            modifier = Modifier.size(20.dp),
        )
        Spacer(Modifier.width(Spacing.md))
        Column {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                color = ParallaxTheme.colors.textPrimary,
            )
            Text(
                text = body,
                style = MaterialTheme.typography.bodySmall,
                color = ParallaxTheme.colors.textSecondary,
            )
        }
    }
}

@Composable
private fun ModeRow(icon: ImageVector, title: String, body: String, available: Boolean) {
    ParallaxCard(bordered = available) {
        Row(Modifier.semantics(mergeDescendants = true) {
            contentDescription = buildString {
                append("$title. $body")
                if (!available) append(" Not available on this device.")
            }
        }) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = if (available) {
                    ParallaxTheme.colors.accent
                } else {
                    ParallaxTheme.colors.textDisabled
                },
                modifier = Modifier.size(24.dp),
            )
            Spacer(Modifier.width(Spacing.md))
            Column {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    color = if (available) {
                        ParallaxTheme.colors.textPrimary
                    } else {
                        ParallaxTheme.colors.textDisabled
                    },
                )
                Text(
                    text = if (available) body else "Needs ARCore, which this device does not have",
                    style = MaterialTheme.typography.bodySmall,
                    color = ParallaxTheme.colors.textTertiary,
                )
            }
        }
    }
}

@Composable
private fun Footer(state: OnboardingUiState, onEvent: (OnboardingEvent) -> Unit) {
    Row(
        Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (state.pageIndex > 0) {
            ParallaxTextButton(text = "Back", onClick = { onEvent(OnboardingEvent.Back) })
        } else {
            ParallaxTextButton(text = "Skip", onClick = { onEvent(OnboardingEvent.Skip) })
        }
        Spacer(Modifier.weight(1f))
        if (state.isLastPage) {
            ParallaxButton(
                text = "Start scanning",
                onClick = { onEvent(OnboardingEvent.Finish) },
                enabled = state.canFinish,
                icon = ParallaxIcons.ChevronRight,
            )
        } else {
            ParallaxButton(
                text = "Next",
                onClick = { onEvent(OnboardingEvent.Next) },
                icon = ParallaxIcons.ChevronRight,
            )
        }
    }
}

/** The app's own entry in system settings, which is where a permanently denied permission
 * can be granted again. */
private fun appSettingsIntent(context: Context): Intent =
    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
        data = Uri.fromParts("package", context.packageName, null)
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }

private const val PAGE_MILLIS = 260
private const val INDICATOR_HEIGHT = 3
private const val HERO_ICON = 48
