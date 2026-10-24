package uk.co.jacoblevy.parallax.feature.ar

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.feature.ar.logic.Placement
import uk.co.jacoblevy.parallax.feature.ar.logic.PlacementPhase
import kotlin.math.abs

class PlacementTest {

    /** A mug: comfortably smaller than a room, so it goes down life-size. */
    private val mug = BoundingBox(-0.05f, 0f, -0.05f, 0.05f, 0.1f, 0.05f)

    /** A room scan whose floor is below its own origin. */
    private val room = BoundingBox(-2f, -0.4f, -3f, 2f, 2.1f, 3f)

    @Test
    fun `it starts looking for a surface and reacts to finding and losing one`() {
        var placement = Placement()
        assertThat(placement.phase).isEqualTo(PlacementPhase.Looking)

        placement = placement.surfaceFound()
        assertThat(placement.phase).isEqualTo(PlacementPhase.Ready)

        placement = placement.surfaceLost()
        assertThat(placement.phase).isEqualTo(PlacementPhase.Looking)
    }

    @Test
    fun `losing a surface after placing does not take the model away`() {
        // ARCore drops and re-finds planes constantly; a model that vanished
        // every time would be unusable.
        val placed = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
        assertThat(placed.surfaceLost().phase).isEqualTo(PlacementPhase.Placed)
    }

    @Test
    fun `a small object is placed life size`() {
        val placement = Placement().surfaceFound().placeAt(1f, 0.8f, -2f, mug)
        assertThat(placement.isPlaced).isTrue()
        assertThat(placement.scale).isEqualTo(1f)
        assertThat(placement.heightMetres).isWithin(1e-4f).of(0.1f)
    }

    @Test
    fun `a room scan is placed as a tabletop model instead of life size`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, room)
        // Showing someone a room scan while standing in a room means a model
        // on the table, not a second room overlapping the first.
        assertThat(placement.scale).isLessThan(1f)
        val longest = maxOf(placement.footprintMetres.first, placement.footprintMetres.second)
        assertThat(longest).isAtMost(Placement.TABLE_TOP_SIZE_METRES + 1e-3f)
    }

    @Test
    fun `the model sits on the surface rather than through it`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0.75f, 0f, room)
        val matrix = placement.modelMatrix()
        // The model's lowest point, after the transform, must land on the
        // tapped surface height.
        val lowest = matrix[5] * room.minY + matrix[7]
        assertThat(lowest).isWithin(1e-4f).of(0.75f)
    }

    @Test
    fun `the transform puts the model where it was tapped`() {
        val placement = Placement().surfaceFound().placeAt(1.5f, 0.2f, -0.5f, mug)
        val matrix = placement.modelMatrix()
        assertThat(matrix[3]).isWithin(1e-5f).of(1.5f)
        assertThat(matrix[11]).isWithin(1e-5f).of(-0.5f)
    }

    @Test
    fun `rotation turns about the vertical axis and nothing else`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
            .rotatedBy((Math.PI / 2).toFloat())
        val matrix = placement.modelMatrix()
        // A quarter turn about Y sends the model's +X to its +Z.
        assertThat(matrix[0]).isWithin(1e-5f).of(0f)
        assertThat(matrix[8]).isWithin(1e-5f).of(-1f)
        assertThat(matrix[2]).isWithin(1e-5f).of(1f)
        // The vertical axis is untouched.
        assertThat(matrix[5]).isWithin(1e-5f).of(1f)
        assertThat(matrix[1]).isEqualTo(0f)
        assertThat(matrix[9]).isEqualTo(0f)
    }

    @Test
    fun `rotation wraps rather than growing without bound`() {
        var placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
        repeat(40) { placement = placement.rotatedBy(1f) }
        assertThat(abs(placement.yaw)).isAtMost(Math.PI.toFloat())
    }

    @Test
    fun `snapping goes to the nearest right angle`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
        val quarter = (Math.PI / 2).toFloat()
        assertThat(placement.rotatedBy(0.2f).snappedToRightAngle().yaw).isWithin(1e-4f).of(0f)
        assertThat(placement.rotatedBy(quarter + 0.2f).snappedToRightAngle().yaw)
            .isWithin(1e-4f).of(quarter)
        assertThat(placement.rotatedBy(quarter - 0.2f).snappedToRightAngle().yaw)
            .isWithin(1e-4f).of(quarter)
    }

    @Test
    fun `scale is bounded at both ends`() {
        var placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
        repeat(50) { placement = placement.scaledBy(2f) }
        assertThat(placement.scale).isAtMost(Placement.MAX_SCALE)
        repeat(100) { placement = placement.scaledBy(0.5f) }
        assertThat(placement.scale).isAtLeast(Placement.MIN_SCALE)
    }

    @Test
    fun `a scale of zero or less is ignored rather than collapsing the model`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug)
        assertThat(placement.scaledBy(0f)).isEqualTo(placement)
        assertThat(placement.scaledBy(-2f)).isEqualTo(placement)
    }

    @Test
    fun `dragging moves across the surface and never off it vertically`() {
        val placement = Placement().surfaceFound().placeAt(0f, 0.5f, 0f, mug)
            .movedBy(0.3f, -0.2f)
        assertThat(placement.anchorX).isWithin(1e-5f).of(0.3f)
        assertThat(placement.anchorZ).isWithin(1e-5f).of(-0.2f)
        assertThat(placement.anchorY).isWithin(1e-5f).of(0.5f)
    }

    @Test
    fun `locking freezes every adjustment`() {
        val locked = Placement().surfaceFound().placeAt(0f, 0f, 0f, mug).withLock(true)
        assertThat(locked.rotatedBy(1f)).isEqualTo(locked)
        assertThat(locked.scaledBy(2f)).isEqualTo(locked)
        assertThat(locked.movedBy(1f, 1f)).isEqualTo(locked)
        assertThat(locked.snappedToRightAngle()).isEqualTo(locked)
        // Unlocking lets it move again.
        assertThat(locked.withLock(false).rotatedBy(1f).yaw).isNotEqualTo(locked.yaw)
    }

    @Test
    fun `resetting goes back to waiting for a tap and keeps the bounds`() {
        val placement = Placement().surfaceFound().placeAt(1f, 1f, 1f, room)
            .rotatedBy(0.6f)
            .scaledBy(2f)
        val reset = placement.reset()
        assertThat(reset.phase).isEqualTo(PlacementPhase.Ready)
        assertThat(reset.yaw).isEqualTo(0f)
        assertThat(reset.scale).isEqualTo(1f)
        assertThat(reset.bounds).isEqualTo(room)
    }

    @Test
    fun `the footprint accounts for rotation`() {
        // A long thin scan turned a quarter turn swaps its width and depth.
        val slab = BoundingBox(0f, 0f, 0f, 2f, 0.1f, 0.5f)
        val placement = Placement().surfaceFound().placeAt(0f, 0f, 0f, slab)
        val square = placement.footprintMetres
        val turned = placement.rotatedBy((Math.PI / 2).toFloat()).footprintMetres
        assertThat(square.first).isWithin(1e-3f).of(turned.second)
        assertThat(square.second).isWithin(1e-3f).of(turned.first)
    }
}
