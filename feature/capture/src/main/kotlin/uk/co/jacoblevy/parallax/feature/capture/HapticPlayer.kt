package uk.co.jacoblevy.parallax.feature.capture

import android.Manifest
import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import androidx.annotation.RequiresPermission
import dagger.hilt.android.qualifiers.ApplicationContext
import uk.co.jacoblevy.parallax.core.model.HapticCue
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Plays the capture screen's haptics.
 *
 * A functional interface so a test can pass a lambda that records what was played: haptics are
 * one of the few things on this screen with no observable result, and asserting that the right
 * cue fired at the right moment is the only way to know the feedback is wired up.
 */
fun interface HapticPlayer {
    fun play(cue: HapticCue, enabled: Boolean)
}

/**
 * Haptics through the platform vibrator.
 *
 * Predefined effects where the device has them, because an OEM's tick feels like the rest of
 * that phone while a hand-rolled 20 ms pulse feels like a cheap app. The fallback waveforms are
 * only used where predefined effects are unsupported.
 */
@Singleton
class SystemHapticPlayer @Inject constructor(
    @ApplicationContext context: Context,
) : HapticPlayer {

    /**
     * The vibrator.
     *
     * `VibratorManager` is the API 31 way and is the only one that gives the right device on a
     * phone with more than one actuator; below that there is a single system vibrator. minSdk
     * is 29, so both paths are needed.
     */
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Vibrator::class.java)
    }

    @RequiresPermission(Manifest.permission.VIBRATE)
    override fun play(cue: HapticCue, enabled: Boolean) {
        if (!enabled || cue == HapticCue.None) return
        val device = vibrator ?: return
        if (!device.hasVibrator()) return

        val effect = when (cue) {
            HapticCue.None -> return
            HapticCue.Hint -> VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK)
            HapticCue.Warning -> VibrationEffect.createPredefined(
                VibrationEffect.EFFECT_DOUBLE_CLICK,
            )
            // Two ticks rising in strength: there is no predefined "success", and a single
            // heavy click reads as an error rather than as completion.
            HapticCue.Success -> VibrationEffect.createWaveform(
                SUCCESS_TIMINGS,
                SUCCESS_AMPLITUDES,
                NO_REPEAT,
            )
            HapticCue.Shutter -> VibrationEffect.createPredefined(
                VibrationEffect.EFFECT_HEAVY_CLICK,
            )
        }
        runCatching { device.vibrate(effect) }
    }

    private companion object {
        val SUCCESS_TIMINGS = longArrayOf(0, 30, 60, 50)
        val SUCCESS_AMPLITUDES = intArrayOf(0, 120, 0, 200)
        const val NO_REPEAT = -1
    }
}
