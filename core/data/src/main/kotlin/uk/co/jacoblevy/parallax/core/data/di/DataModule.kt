package uk.co.jacoblevy.parallax.core.data.di

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.Preferences
import androidx.room.Room
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import uk.co.jacoblevy.parallax.core.data.database.AnnotationDao
import uk.co.jacoblevy.parallax.core.data.database.FolderDao
import uk.co.jacoblevy.parallax.core.data.database.MeasurementDao
import uk.co.jacoblevy.parallax.core.data.database.ParallaxDatabase
import uk.co.jacoblevy.parallax.core.data.database.ProcessingJobDao
import uk.co.jacoblevy.parallax.core.data.database.ProjectDao
import uk.co.jacoblevy.parallax.core.data.repository.IdGenerator
import uk.co.jacoblevy.parallax.core.data.repository.TimeSource
import uk.co.jacoblevy.parallax.core.data.repository.UuidIdGenerator
import uk.co.jacoblevy.parallax.core.data.settings.SettingsRepository
import java.io.File
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object DataModule {

    @Provides
    @Singleton
    fun database(@ApplicationContext context: Context): ParallaxDatabase =
        Room.databaseBuilder(context, ParallaxDatabase::class.java, ParallaxDatabase.NAME)
            // No fallbackToDestructiveMigration: losing a user's library to a schema change is
            // not an acceptable outcome for data that exists nowhere else. Every version bump
            // ships a migration, and the exported schema is what verifies it.
            .build()

    @Provides
    fun projectDao(database: ParallaxDatabase): ProjectDao = database.projectDao()

    @Provides
    fun folderDao(database: ParallaxDatabase): FolderDao = database.folderDao()

    @Provides
    fun annotationDao(database: ParallaxDatabase): AnnotationDao = database.annotationDao()

    @Provides
    fun measurementDao(database: ParallaxDatabase): MeasurementDao = database.measurementDao()

    @Provides
    fun processingJobDao(database: ParallaxDatabase): ProcessingJobDao = database.processingJobDao()

    @Provides
    @Singleton
    fun preferences(@ApplicationContext context: Context): DataStore<Preferences> =
        PreferenceDataStoreFactory.create {
            File(context.filesDir, "datastore/${SettingsRepository.STORE_NAME}.preferences_pb")
                .also { it.parentFile?.mkdirs() }
        }

    @Provides
    @Singleton
    fun timeSource(): TimeSource = TimeSource { System.currentTimeMillis() }

    @Provides
    @Singleton
    fun idGenerator(generator: UuidIdGenerator): IdGenerator = generator
}
