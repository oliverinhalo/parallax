package uk.co.jacoblevy.parallax.navigation

import kotlinx.serialization.Serializable

/**
 * Every screen the app can navigate to.
 *
 * Type-safe routes rather than string templates: a project id spliced into a URL is one typo
 * away from a silent navigation failure, and the compiler cannot help with `"viewer/{id}"`.
 */
sealed interface ParallaxRoute {

    @Serializable
    data object Onboarding : ParallaxRoute

    @Serializable
    data object Library : ParallaxRoute

    @Serializable
    data object Capture : ParallaxRoute

    @Serializable
    data object Settings : ParallaxRoute

    @Serializable
    data object Licences : ParallaxRoute
}
