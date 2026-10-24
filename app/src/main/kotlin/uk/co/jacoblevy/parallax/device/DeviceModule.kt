package uk.co.jacoblevy.parallax.device

import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import uk.co.jacoblevy.parallax.core.model.DeviceCapabilityProvider
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
interface DeviceModule {
    @Binds
    @Singleton
    fun capabilityProvider(
        provider: AndroidDeviceCapabilityProvider,
    ): DeviceCapabilityProvider
}
