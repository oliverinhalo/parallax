package uk.co.jacoblevy.parallax.feature.library

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.produceState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * A project's thumbnail, decoded off the main thread.
 *
 * Deliberately not an image-loading library: there is exactly one image per card, it is a small
 * local JPEG the app wrote itself, and a loader's disk cache would be a second copy of a file
 * that is already on disk. Decoding is bounded by [MAX_DIMENSION] so a card never holds a
 * full-resolution bitmap, and the state is keyed on the file's modification time so a
 * regenerated thumbnail replaces the old one.
 */
@Composable
internal fun ProjectThumbnail(file: File, modifier: Modifier = Modifier) {
    val bitmap: ImageBitmap? by produceState<ImageBitmap?>(
        initialValue = null,
        file.path,
        file.lastModified(),
    ) {
        value = withContext(Dispatchers.IO) { decodeScaled(file) }
    }

    val image = bitmap
    if (image == null) {
        // An empty box rather than a spinner: the placeholder gradient behind this is already
        // the right shape and colour, and a spinner per card would make the grid flicker.
        Box(modifier)
    } else {
        Image(
            bitmap = image,
            contentDescription = null,
            modifier = modifier,
            contentScale = ContentScale.Crop,
        )
    }
}

/**
 * Decode [file] at no more than [MAX_DIMENSION] on its longest side.
 *
 * Two passes: the first reads only the header for the dimensions, the second decodes with the
 * power-of-two sample size that gets closest without going under.
 */
private fun decodeScaled(file: File): ImageBitmap? {
    if (!file.isFile) return null
    return runCatching {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        val longest = maxOf(bounds.outWidth, bounds.outHeight)
        if (longest <= 0) return@runCatching null

        var sample = 1
        while (longest / (sample * 2) >= MAX_DIMENSION) sample *= 2

        val options = BitmapFactory.Options().apply { inSampleSize = sample }
        BitmapFactory.decodeFile(file.path, options)?.asImageBitmap()
    }.getOrNull()
}

private const val MAX_DIMENSION = 512
