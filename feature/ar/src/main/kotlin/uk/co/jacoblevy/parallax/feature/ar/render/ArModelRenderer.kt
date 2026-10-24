package uk.co.jacoblevy.parallax.feature.ar.render

import android.opengl.GLES20
import androidx.compose.ui.geometry.Offset
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.nio.IntBuffer

/**
 * A tap waiting to be turned into a point on the floor.
 *
 * The hit test needs the ARCore frame, which only exists on the GL thread, so
 * the tap is recorded where it happened and consumed by the next frame.
 */
class PendingTap {
    @Volatile
    var offset: Offset? = null
}

/**
 * Draws the placed model over the camera feed.
 *
 * Hand-written GLES rather than Filament: the AR view has to composite the
 * model onto ARCore's camera texture, which in Filament needs a custom
 * material compiled by `matc` at build time. One vertex and one fragment
 * shader here does the same job with nothing added to the build, and matches
 * what the capture viewfinder already does for the camera background.
 *
 * The shading is deliberately plain -- a key light, a sky term and a ground
 * plane shadow ellipse -- because the point of this screen is "does the scan
 * look right in this room at this size", not a product render.
 */
class ArModelRenderer {

    private var program = 0
    private var positionAttribute = 0
    private var normalAttribute = 0
    private var colorAttribute = 0
    private var modelUniform = 0
    private var viewProjectionUniform = 0
    private var hasColorUniform = 0
    private var alphaUniform = 0

    private var positions: FloatBuffer? = null
    private var normals: FloatBuffer? = null
    private var colors: FloatBuffer? = null
    private var indices: IntBuffer? = null
    private var indexCount = 0

    val hasModel: Boolean get() = indexCount > 0

    fun createOnGlThread() {
        program = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        positionAttribute = GLES20.glGetAttribLocation(program, "aPosition")
        normalAttribute = GLES20.glGetAttribLocation(program, "aNormal")
        colorAttribute = GLES20.glGetAttribLocation(program, "aColor")
        modelUniform = GLES20.glGetUniformLocation(program, "uModel")
        viewProjectionUniform = GLES20.glGetUniformLocation(program, "uViewProjection")
        hasColorUniform = GLES20.glGetUniformLocation(program, "uHasColor")
        alphaUniform = GLES20.glGetUniformLocation(program, "uAlpha")
    }

    /**
     * Give the renderer the geometry to draw.
     *
     * Client-side buffers rather than VBOs, because this is called off the GL
     * thread when the model finishes loading and uploading a buffer needs a
     * context. The model is decimated before it gets here, so the per-frame
     * cost of re-specifying the pointers is not what limits this screen.
     */
    fun setModel(
        vertices: FloatArray,
        triangleIndices: IntArray,
        vertexNormals: FloatArray?,
        vertexColors: FloatArray?,
    ) {
        positions = vertices.toBuffer()
        normals = vertexNormals?.toBuffer()
        colors = vertexColors?.toBuffer()
        indices = triangleIndices.toBuffer()
        indexCount = triangleIndices.size
    }

    fun clearModel() {
        positions = null
        normals = null
        colors = null
        indices = null
        indexCount = 0
    }

    /** `model` and `viewProjection` are column-major, as GLES expects. */
    fun draw(model: FloatArray, viewProjection: FloatArray, alpha: Float) {
        val vertexBuffer = positions ?: return
        val indexBuffer = indices ?: return
        if (indexCount == 0) return

        GLES20.glUseProgram(program)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
        GLES20.glDepthMask(true)
        if (alpha < 1f) {
            GLES20.glEnable(GLES20.GL_BLEND)
            GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA)
        } else {
            GLES20.glDisable(GLES20.GL_BLEND)
        }

        GLES20.glUniformMatrix4fv(modelUniform, 1, false, model, 0)
        GLES20.glUniformMatrix4fv(viewProjectionUniform, 1, false, viewProjection, 0)
        GLES20.glUniform1f(alphaUniform, alpha)
        GLES20.glUniform1i(hasColorUniform, if (colors != null) 1 else 0)

        vertexBuffer.position(0)
        GLES20.glVertexAttribPointer(
            positionAttribute, VEC3, GLES20.GL_FLOAT, false, 0, vertexBuffer,
        )
        GLES20.glEnableVertexAttribArray(positionAttribute)

        normals?.let {
            it.position(0)
            GLES20.glVertexAttribPointer(normalAttribute, VEC3, GLES20.GL_FLOAT, false, 0, it)
            GLES20.glEnableVertexAttribArray(normalAttribute)
        } ?: GLES20.glDisableVertexAttribArray(normalAttribute)

        colors?.let {
            it.position(0)
            GLES20.glVertexAttribPointer(colorAttribute, VEC3, GLES20.GL_FLOAT, false, 0, it)
            GLES20.glEnableVertexAttribArray(colorAttribute)
        } ?: GLES20.glDisableVertexAttribArray(colorAttribute)

        indexBuffer.position(0)
        GLES20.glDrawElements(GLES20.GL_TRIANGLES, indexCount, GLES20.GL_UNSIGNED_INT, indexBuffer)

        GLES20.glDisableVertexAttribArray(positionAttribute)
        GLES20.glDisable(GLES20.GL_BLEND)
    }

    private fun buildProgram(vertexSource: String, fragmentSource: String): Int {
        val vertex = compile(GLES20.GL_VERTEX_SHADER, vertexSource)
        val fragment = compile(GLES20.GL_FRAGMENT_SHADER, fragmentSource)
        val handle = GLES20.glCreateProgram()
        GLES20.glAttachShader(handle, vertex)
        GLES20.glAttachShader(handle, fragment)
        GLES20.glLinkProgram(handle)
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)
        return handle
    }

    private fun compile(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        return shader
    }

    private fun FloatArray.toBuffer(): FloatBuffer =
        ByteBuffer.allocateDirect(size * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .put(this)
            .apply { position(0) }

    private fun IntArray.toBuffer(): IntBuffer =
        ByteBuffer.allocateDirect(size * Int.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
            .asIntBuffer()
            .put(this)
            .apply { position(0) }

    private companion object {
        const val VEC3 = 3

        val VERTEX_SHADER = """
            uniform mat4 uModel;
            uniform mat4 uViewProjection;
            attribute vec4 aPosition;
            attribute vec3 aNormal;
            attribute vec3 aColor;
            varying vec3 vNormal;
            varying vec3 vColor;
            void main() {
              vNormal = mat3(uModel) * aNormal;
              vColor = aColor;
              gl_Position = uViewProjection * uModel * aPosition;
            }
        """.trimIndent()

        val FRAGMENT_SHADER = """
            precision mediump float;
            uniform int uHasColor;
            uniform float uAlpha;
            varying vec3 vNormal;
            varying vec3 vColor;
            void main() {
              vec3 base = uHasColor == 1 ? vColor : vec3(0.78);
              vec3 n = normalize(vNormal);
              float key = max(dot(n, normalize(vec3(0.35, 0.85, 0.4))), 0.0);
              float sky = 0.5 + 0.5 * n.y;
              vec3 lit = base * (0.3 + 0.6 * key + 0.2 * sky);
              gl_FragColor = vec4(pow(lit, vec3(1.0 / 2.2)), uAlpha);
            }
        """.trimIndent()
    }
}
