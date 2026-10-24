package uk.co.jacoblevy.parallax.core.common.result

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart

/**
 * The outcome of an operation that can fail in a way the user needs to know about.
 *
 * Deliberately not Kotlin's own [kotlin.Result]: that type carries a bare [Throwable], which
 * tells the UI nothing about what to offer the user next. Every failure here is a
 * [ParallaxError], and every [ParallaxError] carries a recovery action, so an error screen is
 * always actionable rather than a dead end.
 */
sealed interface ParallaxResult<out T> {
    data class Success<T>(val data: T) : ParallaxResult<T>
    data class Failure(val error: ParallaxError) : ParallaxResult<Nothing>
    data object Loading : ParallaxResult<Nothing>

    val isSuccess: Boolean get() = this is Success
    val isFailure: Boolean get() = this is Failure
    val isLoading: Boolean get() = this is Loading

    fun getOrNull(): T? = (this as? Success)?.data
    fun errorOrNull(): ParallaxError? = (this as? Failure)?.error
}

inline fun <T, R> ParallaxResult<T>.map(transform: (T) -> R): ParallaxResult<R> = when (this) {
    is ParallaxResult.Success -> ParallaxResult.Success(transform(data))
    is ParallaxResult.Failure -> this
    ParallaxResult.Loading -> ParallaxResult.Loading
}

inline fun <T, R> ParallaxResult<T>.flatMap(
    transform: (T) -> ParallaxResult<R>,
): ParallaxResult<R> = when (this) {
    is ParallaxResult.Success -> transform(data)
    is ParallaxResult.Failure -> this
    ParallaxResult.Loading -> ParallaxResult.Loading
}

inline fun <T> ParallaxResult<T>.onSuccess(action: (T) -> Unit): ParallaxResult<T> = apply {
    if (this is ParallaxResult.Success) action(data)
}

inline fun <T> ParallaxResult<T>.onFailure(action: (ParallaxError) -> Unit): ParallaxResult<T> =
    apply { if (this is ParallaxResult.Failure) action(error) }

fun <T> ParallaxResult<T>.getOrElse(fallback: (ParallaxError) -> T): T = when (this) {
    is ParallaxResult.Success -> data
    is ParallaxResult.Failure -> fallback(error)
    ParallaxResult.Loading -> fallback(ParallaxError.Unknown("still loading"))
}

/**
 * Wraps a flow so it emits [ParallaxResult.Loading] first and converts thrown exceptions into
 * [ParallaxResult.Failure]. Collectors therefore never need a try/catch, and a repository that
 * throws cannot take the UI down with it.
 */
fun <T> Flow<T>.asParallaxResult(): Flow<ParallaxResult<T>> =
    map<T, ParallaxResult<T>> { ParallaxResult.Success(it) }
        .onStart { emit(ParallaxResult.Loading) }
        .catch { emit(ParallaxResult.Failure(it.toParallaxError())) }

/** Runs [block], converting any exception into a [ParallaxResult.Failure]. */
inline fun <T> runCatchingParallax(block: () -> T): ParallaxResult<T> = try {
    ParallaxResult.Success(block())
} catch (cancellation: kotlinx.coroutines.CancellationException) {
    // Cancellation is cooperative control flow, not an error, and swallowing it would break
    // structured concurrency.
    throw cancellation
} catch (throwable: Throwable) {
    ParallaxResult.Failure(throwable.toParallaxError())
}
