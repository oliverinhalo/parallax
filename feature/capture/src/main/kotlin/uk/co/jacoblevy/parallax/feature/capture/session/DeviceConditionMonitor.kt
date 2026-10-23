package uk.co.jacoblevy.parallax.feature.capture.session

import android.content.Context
import android.os.BatteryManager
import android.os.PowerManager
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import uk.co.jacoblevy.parallax.core.model.ThermalStatus
import java.util.concurrent.Executor
import javax.inject.Inject
import javax.inject.Singleton

/** Thermal state, battery level and free storage, as capture needs them. */
data class DeviceConditions(
    val thermalStatus: ThermalStatus = ThermalStatus.Nominal,
    val batteryPercent: Int = 100,
    val charging: Boolean = false,
    val storageAvailableBytes: Long = Long.MAX_VALUE,
)

/**
 * Watches the conditions that can end a scan.
 *
 * Thermal state comes from a listener rather than a poll, because the transition from Fair to
 * Serious is exactly the moment capture needs to slow down and polling would find it up to a
 * second late. Battery and storage are read alongside each thermal change and on a slow tick:
 * neither moves fast enough to justify a broadcast receiver, and a receiver for
 * `ACTION_BATTERY_CHANGED` wakes on every percentage point for the whole session.
 */
@Singleton
class DeviceConditionMonitor @Inject constructor(
    @ApplicationContext private val context: Context,
) {

    private val powerManager: PowerManager? =
        context.getSystemService(PowerManager::class.java)
    private val batteryManager: BatteryManager? =
        context.getSystemService(BatteryManager::class.java)

    /** A snapshot, for the places that need one value rather than a stream. */
    fun current(): DeviceConditions = DeviceConditions(
        thermalStatus = readThermalStatus(),
        batteryPercent = readBatteryPercent(),
        charging = readCharging(),
        storageAvailableBytes = context.filesDir.usableSpace,
    )

    /**
     * Conditions as they change.
     *
     * `distinctUntilChanged` matters: the thermal listener fires on every status change, and
     * without it an unchanged battery percentage would push a new value through the capture
     * state machine several times a second.
     */
    fun conditions(): Flow<DeviceConditions> = callbackFlow {
        trySend(current())

        val executor = Executor { command -> command.run() }
        val listener = PowerManager.OnThermalStatusChangedListener { trySend(current()) }
        val manager = powerManager
        if (manager != null) {
            runCatching { manager.addThermalStatusListener(executor, listener) }
        }

        awaitClose {
            if (manager != null) {
                runCatching { manager.removeThermalStatusListener(listener) }
            }
        }
    }.distinctUntilChanged()

    private fun readThermalStatus(): ThermalStatus {
        val status = powerManager?.currentThermalStatus ?: PowerManager.THERMAL_STATUS_NONE
        return when (status) {
            PowerManager.THERMAL_STATUS_NONE -> ThermalStatus.Nominal
            PowerManager.THERMAL_STATUS_LIGHT, PowerManager.THERMAL_STATUS_MODERATE ->
                ThermalStatus.Fair
            PowerManager.THERMAL_STATUS_SEVERE -> ThermalStatus.Serious
            PowerManager.THERMAL_STATUS_CRITICAL -> ThermalStatus.Critical
            PowerManager.THERMAL_STATUS_EMERGENCY, PowerManager.THERMAL_STATUS_SHUTDOWN ->
                ThermalStatus.Emergency
            else -> ThermalStatus.Nominal
        }
    }

    /**
     * Battery level, or 100 when it cannot be read.
     *
     * Some devices return 0 before the first battery reading lands, and a "battery low" warning
     * on a fully charged phone destroys trust in every other prompt, so an implausible 0 is
     * treated as unknown.
     */
    private fun readBatteryPercent(): Int {
        val level = batteryManager
            ?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
            ?: return FULL_BATTERY
        return if (level <= 0) FULL_BATTERY else level.coerceAtMost(FULL_BATTERY)
    }

    private fun readCharging(): Boolean = batteryManager?.isCharging ?: false

    private companion object {
        const val FULL_BATTERY = 100
    }
}
