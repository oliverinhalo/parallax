package uk.co.jacoblevy.parallax.core.data.database

import androidx.room.Database
import androidx.room.RoomDatabase

/**
 * The app's only database.
 *
 * There are no migrations yet because version 1 is the first shipped schema, but the generated
 * schema JSON is exported (see the `parallax.android.room` convention plugin) so that the first
 * migration can be written against a real baseline rather than a reconstruction of it.
 */
@Database(
    entities = [
        ProjectEntity::class,
        FolderEntity::class,
        AnnotationEntity::class,
        MeasurementEntity::class,
        ProcessingJobEntity::class,
    ],
    version = 1,
    exportSchema = true,
)
abstract class ParallaxDatabase : RoomDatabase() {
    abstract fun projectDao(): ProjectDao
    abstract fun folderDao(): FolderDao
    abstract fun annotationDao(): AnnotationDao
    abstract fun measurementDao(): MeasurementDao
    abstract fun processingJobDao(): ProcessingJobDao

    companion object {
        const val NAME = "parallax.db"
    }
}
