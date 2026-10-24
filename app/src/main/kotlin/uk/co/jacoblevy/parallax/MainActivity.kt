package uk.co.jacoblevy.parallax

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.launch
import uk.co.jacoblevy.parallax.core.designsystem.theme.ParallaxTheme
import uk.co.jacoblevy.parallax.diagnostics.CrashReporter
import uk.co.jacoblevy.parallax.navigation.ParallaxNavHost
import uk.co.jacoblevy.parallax.navigation.ParallaxRoute

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    private val viewModel: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        val splash = installSplashScreen()
        super.onCreate(savedInstanceState)

        // Showing the crash screen from the handler is a race: the request reaches the system
        // while the process it came from is being torn down, and the screen can be lost. The
        // report on disk is not, so an unread one is shown on the next launch instead -- which
        // is also the only route for a crash the handler could not show anything for at all.
        if (CrashReporter.showStoredReport(this)) {
            finish()
            return
        }

        // Hold the splash until the stored theme and onboarding flag are known: drawing with
        // defaults and correcting afterwards flashes the wrong theme on every cold start. The
        // flag is driven from a collector rather than from composition so that the condition is
        // already settled by the time the first frame is requested.
        var ready = false
        splash.setKeepOnScreenCondition { !ready }
        lifecycleScope.launch {
            viewModel.state.collect { ready = it is AppUiState.Ready }
        }

        // Transparent system bars: the capture viewfinder and the viewer are full-bleed, and a
        // solid bar would cut into both. `auto` picks light or dark icons from the theme.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.auto(Color.TRANSPARENT, Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.auto(Color.TRANSPARENT, Color.TRANSPARENT),
        )

        setContent {
            val state by viewModel.state.collectAsStateWithLifecycle()
            when (val current = state) {
                // Nothing is drawn while loading; the splash is still up.
                AppUiState.Loading -> Unit
                is AppUiState.Ready -> ParallaxTheme(
                    darkTheme = current.settings.useDarkTheme(isSystemInDarkTheme()),
                    dynamicColor = current.settings.useDynamicColor,
                ) {
                    ParallaxNavHost(
                        startDestination = if (current.onboardingComplete) {
                            ParallaxRoute.Library
                        } else {
                            ParallaxRoute.Onboarding
                        },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
    }
}
