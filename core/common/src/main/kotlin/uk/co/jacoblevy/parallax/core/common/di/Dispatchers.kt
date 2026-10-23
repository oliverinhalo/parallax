package uk.co.jacoblevy.parallax.core.common.di

import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Qualifier
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class IoDispatcher

@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class DefaultDispatcher

@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class MainDispatcher

/**
 * The dispatcher reconstruction runs on.
 *
 * Deliberately separate from [DefaultDispatcher]. Reconstruction saturates every core it is
 * given for minutes at a time; sharing Dispatchers.Default would starve ordinary background
 * work -- thumbnail decoding, database queries -- and the UI would visibly stall. The pool is
 * also capped below the core count so the scheduler can still run the render thread.
 */
@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class ReconstructionDispatcher

@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class ApplicationScope

@Module
@InstallIn(SingletonComponent::class)
object DispatchersModule {

    @Provides
    @IoDispatcher
    fun providesIoDispatcher(): CoroutineDispatcher = Dispatchers.IO

    @Provides
    @DefaultDispatcher
    fun providesDefaultDispatcher(): CoroutineDispatcher = Dispatchers.Default

    @Provides
    @MainDispatcher
    fun providesMainDispatcher(): CoroutineDispatcher = Dispatchers.Main.immediate

    @Provides
    @Singleton
    @ReconstructionDispatcher
    fun providesReconstructionDispatcher(): CoroutineDispatcher {
        // Leave at least one core for the UI and the camera pipeline. On a 4-core phone that
        // means 3 reconstruction threads, which is where throughput plateaus anyway because
        // the stages are memory-bandwidth bound rather than compute bound.
        val parallelism = (Runtime.getRuntime().availableProcessors() - 1)
            .coerceIn(1, MAX_RECONSTRUCTION_THREADS)
        return Dispatchers.Default.limitedParallelism(parallelism)
    }

    @Provides
    @Singleton
    @ApplicationScope
    fun providesApplicationScope(
        @DefaultDispatcher dispatcher: CoroutineDispatcher,
    ): CoroutineScope = CoroutineScope(SupervisorJob() + dispatcher)

    /** More than this buys nothing: the stages saturate memory bandwidth first. */
    private const val MAX_RECONSTRUCTION_THREADS = 6
}
