package uk.co.jacoblevy.parallax.feature.viewer

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import uk.co.jacoblevy.parallax.core.model.BoundingBox
import uk.co.jacoblevy.parallax.core.model.Measurement
import uk.co.jacoblevy.parallax.core.model.ModelEdit
import uk.co.jacoblevy.parallax.core.model.Point2
import uk.co.jacoblevy.parallax.core.model.Point3
import uk.co.jacoblevy.parallax.feature.viewer.camera.OrbitCamera
import uk.co.jacoblevy.parallax.feature.viewer.camera.WalkCamera
import uk.co.jacoblevy.parallax.feature.viewer.edit.EditPlan
import uk.co.jacoblevy.parallax.feature.viewer.edit.LassoCut
import uk.co.jacoblevy.parallax.feature.viewer.edit.identityMatrix
import uk.co.jacoblevy.parallax.feature.viewer.edit.levellingRotation
import uk.co.jacoblevy.parallax.feature.viewer.edit.planFor
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureDraft
import uk.co.jacoblevy.parallax.feature.viewer.measure.MeasureTool
import uk.co.jacoblevy.parallax.feature.viewer.measure.formatArea
import uk.co.jacoblevy.parallax.feature.viewer.measure.formatLength
import uk.co.jacoblevy.parallax.feature.viewer.measure.polygonAreaSquareMetres
import uk.co.jacoblevy.parallax.feature.viewer.pick.Ray
import uk.co.jacoblevy.parallax.feature.viewer.pick.TrianglePicker
import uk.co.jacoblevy.parallax.feature.viewer.pick.screenRay
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.sqrt

class OrbitCameraTest {

    private val unitBox = BoundingBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f)

    @Test
    fun `the eye sits at the orbit distance from the target`() {
        val camera = OrbitCamera(targetX = 1f, targetY = 2f, targetZ = 3f, distance = 4f)
        val separation = hypot(
            hypot(camera.eyeX - 1f, camera.eyeY - 2f),
            camera.eyeZ - 3f,
        )
        assertThat(separation).isWithin(1e-4f).of(4f)
    }

    @Test
    fun `framing a box puts the whole of it inside the field of view`() {
        val camera = OrbitCamera.framing(unitBox, aspectRatio = 0.5f)
        // Every corner has to project inside the frustum, or the model is cut
        // off the moment the view opens.
        val halfFov = 0.5f * camera.fieldOfViewDegrees * OrbitCamera.DEGREES_TO_RADIANS
        for (x in listOf(unitBox.minX, unitBox.maxX)) {
            for (y in listOf(unitBox.minY, unitBox.maxY)) {
                for (z in listOf(unitBox.minZ, unitBox.maxZ)) {
                    val toCorner = sqrt(
                        (x - camera.eyeX) * (x - camera.eyeX) +
                            (y - camera.eyeY) * (y - camera.eyeY) +
                            (z - camera.eyeZ) * (z - camera.eyeZ),
                    )
                    val toTarget = camera.distance
                    // Angle between the view axis and the corner, by the law of cosines.
                    val radius = 0.5f * sqrt(3f)
                    val cosine =
                        (toTarget * toTarget + toCorner * toCorner - radius * radius) /
                            (2f * toTarget * toCorner)
                    val angle = kotlin.math.acos(cosine.coerceIn(-1f, 1f))
                    assertThat(angle).isLessThan(halfFov * 0.5f + halfFov)
                }
            }
        }
    }

    @Test
    fun `framing scales the distance with the model`() {
        val small = OrbitCamera.framing(unitBox)
        val large = OrbitCamera.framing(BoundingBox(-5f, -5f, -5f, 5f, 5f, 5f))
        assertThat(large.distance).isWithin(0.02f).of(small.distance * 10f)
    }

    @Test
    fun `pitch stops short of the poles`() {
        var camera = OrbitCamera.framing(unitBox)
        repeat(100) { camera = camera.rotatedBy(0f, 1f) }
        assertThat(camera.pitch).isAtMost(OrbitCamera.MAX_PITCH)
        // Exactly at the pole the up vector degenerates and the model spins.
        assertThat(camera.pitch).isLessThan((Math.PI / 2).toFloat())

        repeat(200) { camera = camera.rotatedBy(0f, -1f) }
        assertThat(camera.pitch).isAtLeast(-OrbitCamera.MAX_PITCH)
    }

    @Test
    fun `yaw wraps rather than growing without bound`() {
        var camera = OrbitCamera.framing(unitBox)
        repeat(50) { camera = camera.rotatedBy(1f, 0f) }
        assertThat(abs(camera.yaw)).isAtMost(Math.PI.toFloat())
    }

    @Test
    fun `zoom is bounded at both ends`() {
        var camera = OrbitCamera.framing(unitBox)
        repeat(60) { camera = camera.zoomedBy(2f) }
        assertThat(camera.distance).isAtLeast(camera.minDistance)
        repeat(120) { camera = camera.zoomedBy(0.5f) }
        assertThat(camera.distance).isAtMost(camera.maxDistance)
    }

    @Test
    fun `a zoom of one changes nothing and a negative scale is ignored`() {
        val camera = OrbitCamera.framing(unitBox)
        assertThat(camera.zoomedBy(1f).distance).isEqualTo(camera.distance)
        assertThat(camera.zoomedBy(-2f)).isEqualTo(camera)
    }

    @Test
    fun `panning moves the target across the view, not along it`() {
        val camera = OrbitCamera.framing(unitBox)
        val panned = camera.pannedBy(0.25f, 0f)
        val moveX = panned.targetX - camera.targetX
        val moveY = panned.targetY - camera.targetY
        val moveZ = panned.targetZ - camera.targetZ
        assertThat(hypot(hypot(moveX, moveY), moveZ)).isGreaterThan(0f)

        // The movement must be perpendicular to the viewing direction.
        val viewX = camera.targetX - camera.eyeX
        val viewY = camera.targetY - camera.eyeY
        val viewZ = camera.targetZ - camera.eyeZ
        val dot = moveX * viewX + moveY * viewY + moveZ * viewZ
        assertThat(abs(dot)).isLessThan(1e-3f)
    }

    @Test
    fun `panning further at a greater distance covers more ground`() {
        val near = OrbitCamera.framing(unitBox).copy(distance = 1f)
        val far = near.copy(distance = 10f)
        val nearMove = near.pannedBy(0.2f, 0f).let { hypot(it.targetX - near.targetX, it.targetZ - near.targetZ) }
        val farMove = far.pannedBy(0.2f, 0f).let { hypot(it.targetX - far.targetX, it.targetZ - far.targetZ) }
        assertThat(farMove).isGreaterThan(nearMove * 5f)
    }

    @Test
    fun `a flick keeps turning and then settles`() {
        var camera = OrbitCamera.framing(unitBox).withFling(3f, 0f)
        assertThat(camera.isSettled).isFalse()
        val startingYaw = camera.yaw
        camera = camera.advanced(0.016f)
        assertThat(camera.yaw).isNotEqualTo(startingYaw)

        repeat(600) { camera = camera.advanced(0.016f) }
        assertThat(camera.isSettled).isTrue()
        assertThat(camera.yawVelocity).isEqualTo(0f)
    }

    @Test
    fun `inertia does not depend on the frame rate`() {
        val fling = OrbitCamera.framing(unitBox).copy(yaw = 0f).withFling(2f, 0f)
        var smooth = fling
        repeat(60) { smooth = smooth.advanced(1f / 60f) }
        var stuttery = fling
        repeat(15) { stuttery = stuttery.advanced(1f / 15f) }
        // A long frame must not overshoot into a reversal, and after a second
        // both should have turned by about the same amount.
        assertThat(stuttery.yaw).isWithin(0.08f).of(smooth.yaw)
        assertThat(stuttery.yawVelocity).isGreaterThan(0f)
    }

    @Test
    fun `advancing a settled camera is a no-op`() {
        val camera = OrbitCamera.framing(unitBox)
        assertThat(camera.advanced(0.016f)).isEqualTo(camera.stopped())
    }
}

class WalkCameraTest {

    private val room = BoundingBox(-2f, 0f, -3f, 2f, 2.5f, 3f)

    @Test
    fun `standing in a room starts in the middle at eye height`() {
        val camera = WalkCamera.standingIn(room)
        assertThat(camera.x).isWithin(1e-4f).of(0f)
        assertThat(camera.z).isWithin(1e-4f).of(0f)
        assertThat(camera.y).isWithin(1e-4f).of(WalkCamera.DEFAULT_EYE_HEIGHT)
    }

    @Test
    fun `it starts facing down the long axis of the room`() {
        // This room is deeper than it is wide, so the opening view looks along Z.
        val lengthways = WalkCamera.standingIn(room)
        assertThat(lengthways.forwardZ).isGreaterThan(abs(lengthways.forwardX))

        val wide = WalkCamera.standingIn(BoundingBox(-5f, 0f, -1f, 5f, 2.5f, 1f))
        assertThat(abs(wide.forwardX)).isGreaterThan(abs(wide.forwardZ))
    }

    @Test
    fun `walking cannot leave the scan behind`() {
        var camera = WalkCamera.standingIn(room)
        repeat(200) { camera = camera.moved(forward = 0.5f, strafe = 0.5f) }
        assertThat(camera.x).isAtMost(room.maxX + WalkCamera.MARGIN_METRES)
        assertThat(camera.z).isAtMost(room.maxZ + WalkCamera.MARGIN_METRES)

        repeat(400) { camera = camera.moved(forward = -0.5f, strafe = -0.5f) }
        assertThat(camera.x).isAtLeast(room.minX - WalkCamera.MARGIN_METRES)
        assertThat(camera.z).isAtLeast(room.minZ - WalkCamera.MARGIN_METRES)
    }

    @Test
    fun `looking at the floor does not drive the camera into it`() {
        val camera = WalkCamera.standingIn(room).looked(0f, -1.2f)
        val walked = camera.moved(forward = 1f, strafe = 0f)
        assertThat(walked.y).isEqualTo(camera.y)
    }

    @Test
    fun `pitch is clamped so forward stays meaningful`() {
        var camera = WalkCamera.standingIn(room)
        repeat(50) { camera = camera.looked(0f, 1f) }
        assertThat(camera.pitch).isAtMost(WalkCamera.MAX_PITCH)
    }

    @Test
    fun `eye height is clamped and measured from the floor`() {
        val camera = WalkCamera.standingIn(room).withEyeHeight(9f)
        assertThat(camera.eyeHeightMetres).isEqualTo(WalkCamera.MAX_EYE_HEIGHT)
        assertThat(camera.y).isWithin(1e-4f).of(room.minY + WalkCamera.MAX_EYE_HEIGHT)
    }
}

class RayPickerTest {

    /** An axis-aligned unit cube centred on the origin, as twelve triangles. */
    private fun cube(): Pair<FloatArray, IntArray> {
        val h = 0.5f
        val positions = floatArrayOf(
            -h, -h, -h, h, -h, -h, h, h, -h, -h, h, -h,
            -h, -h, h, h, -h, h, h, h, h, -h, h, h,
        )
        val indices = intArrayOf(
            0, 2, 1, 0, 3, 2, // back  (-z)
            4, 5, 6, 4, 6, 7, // front (+z)
            0, 1, 5, 0, 5, 4, // bottom
            3, 7, 6, 3, 6, 2, // top
            0, 4, 7, 0, 7, 3, // left
            1, 2, 6, 1, 6, 5, // right
        )
        return positions to indices
    }

    @Test
    fun `a ray down the axis hits the near face`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        val hit = picker.intersect(Ray(0f, 0f, 3f, 0f, 0f, -1f))
        assertThat(hit).isNotNull()
        assertThat(hit!!.point.z).isWithin(1e-3f).of(0.5f)
        assertThat(hit.distance).isWithin(1e-3f).of(2.5f)
    }

    @Test
    fun `the normal faces the camera`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        for (direction in listOf(
            Triple(0f, 0f, -1f), Triple(0f, 0f, 1f),
            Triple(-1f, 0f, 0f), Triple(1f, 0f, 0f),
            Triple(0f, -1f, 0f), Triple(0f, 1f, 0f),
        )) {
            val (dx, dy, dz) = direction
            val ray = Ray(-dx * 3f, -dy * 3f, -dz * 3f, dx, dy, dz)
            val hit = picker.intersect(ray)
            assertThat(hit).isNotNull()
            val facing = hit!!.normal.x * dx + hit.normal.y * dy + hit.normal.z * dz
            assertThat(facing).isLessThan(0f)
        }
    }

    @Test
    fun `a ray that misses returns nothing`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        assertThat(picker.intersect(Ray(5f, 5f, 5f, 0f, 0f, -1f))).isNull()
    }

    @Test
    fun `the nearest face wins`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        val hit = picker.intersect(Ray(0f, 0f, -3f, 0f, 0f, 1f))
        assertThat(hit).isNotNull()
        // Entering from -z, the back face at z = -0.5 is the one in front.
        assertThat(hit!!.point.z).isWithin(1e-3f).of(-0.5f)
    }

    @Test
    fun `an empty mesh picks nothing rather than crashing`() {
        val picker = TrianglePicker(FloatArray(0), IntArray(0))
        assertThat(picker.intersect(Ray(0f, 0f, 1f, 0f, 0f, -1f))).isNull()
    }

    @Test
    fun `the centre of the screen looks at what the camera is pointed at`() {
        val camera = OrbitCamera.framing(BoundingBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f))
        val ray = screenRay(camera, 1080, 1920, 540f, 960f)
        // The ray through the middle must run from the eye to the target.
        val toTarget = floatArrayOf(
            camera.targetX - camera.eyeX,
            camera.targetY - camera.eyeY,
            camera.targetZ - camera.eyeZ,
        )
        val length = sqrt(toTarget[0] * toTarget[0] + toTarget[1] * toTarget[1] + toTarget[2] * toTarget[2])
        assertThat(ray.directionX).isWithin(1e-3f).of(toTarget[0] / length)
        assertThat(ray.directionY).isWithin(1e-3f).of(toTarget[1] / length)
        assertThat(ray.directionZ).isWithin(1e-3f).of(toTarget[2] / length)
    }

    @Test
    fun `tapping the centre of the screen hits the model under it`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        val camera = OrbitCamera.framing(BoundingBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f))
        val hit = picker.intersect(screenRay(camera, 1080, 1920, 540f, 960f))
        assertThat(hit).isNotNull()
    }

    @Test
    fun `tapping off to one side hits a different place than the centre`() {
        val (positions, indices) = cube()
        val picker = TrianglePicker(positions, indices)
        val camera = OrbitCamera.framing(BoundingBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f))
        val centre = picker.intersect(screenRay(camera, 1080, 1920, 540f, 960f))
        val offset = picker.intersect(screenRay(camera, 1080, 1920, 700f, 960f))
        assertThat(centre).isNotNull()
        assertThat(offset).isNotNull()
        assertThat(offset!!.point).isNotEqualTo(centre!!.point)
    }
}

class MeasuringTest {

    @Test
    fun `a distance completes on the second point`() {
        var draft = MeasureDraft(MeasureTool.Distance)
        assertThat(draft.isComplete).isFalse()
        draft = draft.withPoint(Point3(0f, 0f, 0f))
        assertThat(draft.isComplete).isFalse()
        draft = draft.withPoint(Point3(3f, 4f, 0f))
        assertThat(draft.isComplete).isTrue()
        assertThat(draft.previewMetres()).isWithin(1e-6).of(5.0)

        val measurement = draft.build("m1", "Width") as Measurement.Distance
        assertThat(measurement.metres).isWithin(1e-6).of(5.0)
    }

    @Test
    fun `a third tap on a two-point tool starts again from it`() {
        val draft = MeasureDraft(MeasureTool.Distance)
            .withPoint(Point3(0f, 0f, 0f))
            .withPoint(Point3(1f, 0f, 0f))
            .withPoint(Point3(5f, 0f, 0f))
        assertThat(draft.points).containsExactly(Point3(5f, 0f, 0f))
    }

    @Test
    fun `height ignores horizontal separation`() {
        val draft = MeasureDraft(MeasureTool.Height)
            .withPoint(Point3(0f, 0.2f, 0f))
            .withPoint(Point3(9f, 2.6f, 9f))
        assertThat(draft.previewMetres()).isWithin(1e-5).of(2.4)
        val measurement = draft.build("m2", "Ceiling") as Measurement.Height
        assertThat(measurement.metres).isWithin(1e-5).of(2.4)
    }

    @Test
    fun `area accumulates and never completes itself`() {
        var draft = MeasureDraft(MeasureTool.Area)
        for (point in listOf(
            Point3(0f, 0f, 0f), Point3(2f, 0f, 0f), Point3(2f, 0f, 3f), Point3(0f, 0f, 3f),
        )) {
            draft = draft.withPoint(point)
            assertThat(draft.isComplete).isFalse()
        }
        assertThat(draft.canFinish).isTrue()
        assertThat(draft.previewMetres()).isWithin(1e-5).of(6.0)
    }

    @Test
    fun `undo removes the last point only`() {
        val draft = MeasureDraft(MeasureTool.Area)
            .withPoint(Point3(0f, 0f, 0f))
            .withPoint(Point3(1f, 0f, 0f))
            .withoutLastPoint()
        assertThat(draft.points).hasSize(1)
    }

    @Test
    fun `an incomplete draft builds nothing`() {
        assertThat(MeasureDraft(MeasureTool.Distance).build("x", "x")).isNull()
        assertThat(
            MeasureDraft(MeasureTool.Area).withPoint(Point3(0f, 0f, 0f)).build("x", "x"),
        ).isNull()
    }

    @Test
    fun `area is right for a polygon that is not axis aligned`() {
        // A unit square tilted 45 degrees about the X axis still has area one.
        val s = (1.0 / sqrt(2.0)).toFloat()
        val area = polygonAreaSquareMetres(
            listOf(
                Point3(0f, 0f, 0f),
                Point3(1f, 0f, 0f),
                Point3(1f, s, s),
                Point3(0f, s, s),
            ),
        )
        assertThat(area).isWithin(1e-5).of(1.0)
    }

    @Test
    fun `area of a degenerate polygon is zero rather than an error`() {
        assertThat(polygonAreaSquareMetres(emptyList())).isEqualTo(0.0)
        assertThat(polygonAreaSquareMetres(listOf(Point3(0f, 0f, 0f), Point3(1f, 0f, 0f))))
            .isEqualTo(0.0)
        assertThat(
            polygonAreaSquareMetres(
                listOf(Point3(0f, 0f, 0f), Point3(1f, 0f, 0f), Point3(2f, 0f, 0f)),
            ),
        ).isWithin(1e-9).of(0.0)
    }

    @Test
    fun `lengths change unit so the number stays readable`() {
        assertThat(formatLength(2.345, imperial = false)).isEqualTo("2.35 m")
        assertThat(formatLength(0.043, imperial = false)).isEqualTo("4.3 cm")
        assertThat(formatLength(0.004, imperial = false)).isEqualTo("4 mm")
    }

    @Test
    fun `imperial lengths read as feet and inches`() {
        assertThat(formatLength(1.0, imperial = true)).isEqualTo("3′ 3.4″")
        assertThat(formatLength(0.1, imperial = true)).isEqualTo("3.94″")
    }

    @Test
    fun `areas carry their unit`() {
        assertThat(formatArea(6.0, imperial = false)).isEqualTo("6.00 m²")
        assertThat(formatArea(6.0, imperial = true)).isEqualTo("64.6 sq ft")
    }

    @Test
    fun `formatting does not depend on the device locale`() {
        val previous = java.util.Locale.getDefault()
        try {
            // A locale that writes a comma for the decimal separator; an
            // exported measurement must not change shape because of it.
            java.util.Locale.setDefault(java.util.Locale.GERMANY)
            assertThat(formatLength(2.5, imperial = false)).isEqualTo("2.50 m")
        } finally {
            java.util.Locale.setDefault(previous)
        }
    }
}

class EditPlanTest {

    @Test
    fun `no edits is the identity`() {
        val plan = planFor(emptyList())
        assertThat(plan.isIdentity).isTrue()
        assertThat(plan.keeps(99f, 99f, 99f)).isTrue()
    }

    @Test
    fun `a crop box keeps what is inside it`() {
        val plan = planFor(listOf(ModelEdit.CropBox(BoundingBox(-1f, -1f, -1f, 1f, 1f, 1f))))
        assertThat(plan.keeps(0f, 0f, 0f)).isTrue()
        assertThat(plan.keeps(1.5f, 0f, 0f)).isFalse()
    }

    @Test
    fun `two crop boxes intersect rather than replacing each other`() {
        val plan = planFor(
            listOf(
                ModelEdit.CropBox(BoundingBox(-1f, -1f, -1f, 1f, 1f, 1f)),
                ModelEdit.CropBox(BoundingBox(0f, -1f, -1f, 2f, 1f, 1f)),
            ),
        )
        assertThat(plan.keeps(0.5f, 0f, 0f)).isTrue()
        assertThat(plan.keeps(-0.5f, 0f, 0f)).isFalse()
        assertThat(plan.keeps(1.5f, 0f, 0f)).isFalse()
    }

    @Test
    fun `rescaling multiplies every coordinate`() {
        val plan = planFor(listOf(ModelEdit.Rescale(measuredMetres = 2.0, actualMetres = 5.0)))
        val point = plan.transformed(1f, 2f, 3f)
        assertThat(point[0]).isWithin(1e-5f).of(2.5f)
        assertThat(point[1]).isWithin(1e-5f).of(5f)
        assertThat(point[2]).isWithin(1e-5f).of(7.5f)
    }

    @Test
    fun `a rescale from nothing is ignored rather than collapsing the model`() {
        val plan = planFor(listOf(ModelEdit.Rescale(measuredMetres = 0.0, actualMetres = 5.0)))
        assertThat(plan.transform).isEqualTo(identityMatrix())
    }

    @Test
    fun `re-levelling turns a tilted floor flat`() {
        // A floor whose normal leans 20 degrees away from vertical.
        val angle = 20.0 * Math.PI / 180.0
        val nx = kotlin.math.sin(angle).toFloat()
        val ny = kotlin.math.cos(angle).toFloat()
        val plan = planFor(listOf(ModelEdit.Relevel(levellingRotation(nx, ny, 0f))))
        val levelled = plan.transformed(nx, ny, 0f)
        assertThat(levelled[0]).isWithin(1e-4f).of(0f)
        assertThat(levelled[1]).isWithin(1e-4f).of(1f)
        assertThat(levelled[2]).isWithin(1e-4f).of(0f)
    }

    @Test
    fun `a floor detected upside down is not levelled by flipping the model`() {
        val plan = planFor(listOf(ModelEdit.Relevel(levellingRotation(0f, -1f, 0f))))
        // Already vertical, just inverted: nothing should move.
        val point = plan.transformed(1f, 2f, 3f)
        assertThat(point[1]).isWithin(1e-4f).of(2f)
    }

    @Test
    fun `re-levelling an already level floor changes nothing`() {
        val plan = planFor(listOf(ModelEdit.Relevel(levellingRotation(0f, 1f, 0f))))
        assertThat(plan.transform).isEqualTo(identityMatrix())
    }

    @Test
    fun `a lasso removes what was inside it on screen`() {
        // A perspective projection, in the sense that matters here: the last
        // row puts depth into w, so a point behind the camera gets a negative
        // one and is rejected rather than being projected to a mirror image.
        val projection = floatArrayOf(
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            0f, 0f, 1f, 0f,
        )
        val square = listOf(
            Point2(-0.5f, -0.5f), Point2(0.5f, -0.5f), Point2(0.5f, 0.5f), Point2(-0.5f, 0.5f),
        )
        val cut = LassoCut(square, projection)
        assertThat(cut.contains(0f, 0f, 1f)).isTrue()
        assertThat(cut.contains(0.9f, 0f, 1f)).isFalse()
        // Behind the camera it was never visible, so it was never selected.
        assertThat(cut.contains(0f, 0f, -1f)).isFalse()
    }

    @Test
    fun `a lasso with too few points is discarded`() {
        val plan = planFor(
            listOf(
                ModelEdit.LassoDelete(
                    screenPolygon = listOf(Point2(0f, 0f), Point2(1f, 1f)),
                    viewMatrix = List(16) { 0f },
                ),
            ),
        )
        assertThat(plan.lassoCuts).isEmpty()
    }

    @Test
    fun `a malformed matrix is discarded rather than corrupting the plan`() {
        val plan = planFor(
            listOf(
                ModelEdit.Relevel(rotation = listOf(1f, 2f)),
                ModelEdit.LassoDelete(
                    screenPolygon = List(4) { Point2(it.toFloat(), it.toFloat()) },
                    viewMatrix = listOf(1f),
                ),
            ),
        )
        assertThat(plan.isIdentity).isTrue()
    }

    @Test
    fun `texture adjustments are carried through for the renderer`() {
        val plan = planFor(
            listOf(ModelEdit.TextureAdjust(exposureStops = 0.5f, saturation = 1.2f, warmth = -0.1f)),
        )
        assertThat(plan.exposureStops).isEqualTo(0.5f)
        assertThat(plan.saturation).isEqualTo(1.2f)
        assertThat(plan.warmth).isEqualTo(-0.1f)
        assertThat(plan.isIdentity).isFalse()
    }

    @Test
    fun `later edits compose onto earlier ones`() {
        val plan = planFor(
            listOf(
                ModelEdit.Rescale(measuredMetres = 1.0, actualMetres = 2.0),
                ModelEdit.Rescale(measuredMetres = 1.0, actualMetres = 3.0),
            ),
        )
        assertThat(plan.transformed(1f, 0f, 0f)[0]).isWithin(1e-4f).of(6f)
    }

    @Test
    fun `plans with the same content are equal`() {
        val edits = listOf(ModelEdit.CropBox(BoundingBox(0f, 0f, 0f, 1f, 1f, 1f)))
        assertThat(planFor(edits)).isEqualTo(planFor(edits))
        assertThat(planFor(edits).hashCode()).isEqualTo(planFor(edits).hashCode())
        assertThat(EditPlan()).isEqualTo(EditPlan())
    }
}
