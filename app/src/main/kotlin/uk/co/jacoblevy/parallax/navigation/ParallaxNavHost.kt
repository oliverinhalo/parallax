package uk.co.jacoblevy.parallax.navigation

import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import uk.co.jacoblevy.parallax.feature.capture.ui.CaptureRoute
import uk.co.jacoblevy.parallax.feature.library.LibraryRoute
import uk.co.jacoblevy.parallax.feature.onboarding.OnboardingRoute
import uk.co.jacoblevy.parallax.feature.settings.LicencesRoute
import uk.co.jacoblevy.parallax.feature.settings.SettingsRoute

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
            LibraryRoute(
                onOpenProject = { /* wired when the viewer lands */ },
                onStartCapture = { navController.navigate(ParallaxRoute.Capture) },
                onOpenSettings = { navController.navigate(ParallaxRoute.Settings) },
                onOpenQueue = { /* wired when processing lands */ },
            )
        }

        composable<ParallaxRoute.Capture> {
            CaptureRoute(
                // A finished capture goes back to the library, which shows the new scan with
                // its "not processed" badge. Popping capture off the stack matters: pressing
                // back from the library must not reopen the camera on a finished scan.
                onFinished = {
                    navController.navigate(ParallaxRoute.Library) {
                        popUpTo(ParallaxRoute.Library) { inclusive = true }
                    }
                },
                onCancelled = navController::popBackStack,
            )
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
