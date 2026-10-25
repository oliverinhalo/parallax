package uk.co.jacoblevy.parallax.navigation

import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.navigation.compose.rememberNavController
import uk.co.jacoblevy.parallax.feature.capture.ui.CaptureRoute
import uk.co.jacoblevy.parallax.feature.library.LibraryRoute
import uk.co.jacoblevy.parallax.feature.processing.ProcessingRoute
import uk.co.jacoblevy.parallax.feature.onboarding.OnboardingRoute
import uk.co.jacoblevy.parallax.feature.settings.LicencesRoute
import uk.co.jacoblevy.parallax.feature.settings.SettingsRoute
import uk.co.jacoblevy.parallax.feature.ar.ui.ArPlacementRoute
import uk.co.jacoblevy.parallax.feature.export.ExportRoute
import uk.co.jacoblevy.parallax.feature.viewer.ui.ViewerRoute

/**
 * The app's navigation graph.
 *
 * Transitions are a short scale-and-fade rather than the platform's lateral slide. The library
 * and the viewer are two views of the same object, so a slide would imply they sit side by side;
 * growing out of the grid matches the shared-element motion the design direction asks for.
 */
@Composable
fun ParallaxNavHost(
    startDestination: ParallaxRoute,
    modifier: Modifier = Modifier,
    navController: NavHostController = rememberNavController(),
) {
    NavHost(
        navController = navController,
        startDestination = startDestination,
        modifier = modifier,
        enterTransition = {
            scaleIn(tween(TRANSITION_MILLIS), initialScale = ENTER_SCALE) +
                fadeIn(tween(TRANSITION_MILLIS))
        },
        exitTransition = { fadeOut(tween(TRANSITION_MILLIS)) },
        popEnterTransition = { fadeIn(tween(TRANSITION_MILLIS)) },
        popExitTransition = {
            scaleOut(tween(TRANSITION_MILLIS), targetScale = ENTER_SCALE) +
                fadeOut(tween(TRANSITION_MILLIS))
        },
    ) {
        composable<ParallaxRoute.Onboarding> {
            OnboardingRoute(
                onFinished = {
                    // Replace rather than push: returning to onboarding with the back button
                    // after finishing it would be nonsense.
                    navController.navigate(ParallaxRoute.Library) {
                        popUpTo(ParallaxRoute.Onboarding) { inclusive = true }
                    }
                },
            )
        }

        composable<ParallaxRoute.Library> {
            val launcher: ProjectLauncherViewModel = hiltViewModel()
            LaunchedEffect(launcher) {
                launcher.destinations.collect { destination ->
                    when (destination) {
                        is ProjectDestination.Viewer ->
                            navController.navigate(ParallaxRoute.Viewer(destination.projectId))
                        ProjectDestination.Processing,
                        ProjectDestination.AlreadyProcessing ->
                            navController.navigate(ParallaxRoute.Processing)
                        is ProjectDestination.ResumeCapture ->
                            navController.navigate(ParallaxRoute.Capture)
                        is ProjectDestination.Failed -> Unit // surfaced by the library's state
                    }
                }
            }
            LibraryRoute(
                onOpenProject = launcher::open,
                onStartCapture = { navController.navigate(ParallaxRoute.Capture) },
                onOpenSettings = { navController.navigate(ParallaxRoute.Settings) },
                onOpenQueue = { navController.navigate(ParallaxRoute.Processing) },
            )
        }

        composable<ParallaxRoute.Capture> {
            CaptureRoute(
                // A finished capture goes back to the library, which shows the new scan with
                // its "not processed" badge. Popping capture off the stack matters: pressing
                // back from the library must not reopen the camera on a finished scan.
                // A finished capture goes straight to the queue, which is where the
                // progress is: bouncing back to the library would make the user hunt for
                // the scan they just took.
                onFinished = {
                    navController.navigate(ParallaxRoute.Processing) {
                        popUpTo(ParallaxRoute.Library)
                    }
                },
                onCancelled = navController::popBackStack,
            )
        }

        composable<ParallaxRoute.Processing> {
            ProcessingRoute(
                onBack = navController::popBackStack,
                onOpenProject = { navController.navigate(ParallaxRoute.Viewer(it)) },
            )
        }

        composable<ParallaxRoute.Viewer> {
            ViewerRoute(
                onBack = navController::popBackStack,
                onExport = { navController.navigate(ParallaxRoute.Export(it)) },
                onPlaceInRoom = { navController.navigate(ParallaxRoute.ArPlacement(it)) },
            )
        }

        composable<ParallaxRoute.Export> {
            ExportRoute(onBack = navController::popBackStack)
        }

        composable<ParallaxRoute.ArPlacement> {
            ArPlacementRoute(onBack = navController::popBackStack)
        }

        composable<ParallaxRoute.Settings> {
            SettingsRoute(
                onBack = navController::popBackStack,
                onOpenLicences = { navController.navigate(ParallaxRoute.Licences) },
            )
        }

        composable<ParallaxRoute.Licences> {
            LicencesRoute(onBack = navController::popBackStack)
        }
    }
}

private const val TRANSITION_MILLIS = 220
private const val ENTER_SCALE = 0.94f
