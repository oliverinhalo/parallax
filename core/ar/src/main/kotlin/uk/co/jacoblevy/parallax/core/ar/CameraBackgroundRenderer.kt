package uk.co.jacoblevy.parallax.core.ar

import android.opengl.GLES11Ext
import android.opengl.GLES20
import com.google.ar.core.Coordinates2d
import com.google.ar.core.Frame
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * Draws ARCore's camera image as the background of the viewfinder.
 *
 * ARCore hands the camera feed over as an external OES texture, which no Compose primitive can
 * draw, so there is a GL surface underneath the UI whose only job is this full-screen quad.
 * Everything else the capture screen shows -- coverage, prompts, stats, controls -- is Compose
 * on top, which keeps the GL code to one shader and twenty lines of geometry.
 */
class CameraBackgroundRenderer {

    private var programHandle = 0
    private var positionAttribute = 0
    private var texCoordAttribute = 0
    private var textureUniform = 0

    /** The texture ARCore writes the camera image into. */
    var textureId: Int = 0
        private set

    private val quadCoords: FloatBuffer = floatBuffer(
        floatArrayOf(
            -1f, -1f,
            +1f, -1f,
            -1f, +1f,
            +1f, +1f,
        ),
    )

    /**
     * Texture coordinates for the quad.
     *
     * Recomputed whenever the display geometry changes, because the camera image's orientation
     * and aspect do not match the view's. ARCore does that mapping itself through
     * [Frame.transformCoordinates2d]; computing it by hand from the rotation is where the
     * classic "preview is sideways on one phone" bug comes from.
     */
    private val textureCoords: FloatBuffer = floatBuffer(FloatArray(QUAD_VERTICES * 2))

    /** Call on the GL thread, once the context exists. */
    fun createOnGlThread() {
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        // The camera texture is sampled once per fragment at close to 1:1, so linear filtering
        // and clamped edges are all it needs; mipmaps would be generated every frame for nothing.
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_WRAP_S,
            GLES20.GL_CLAMP_TO_EDGE,
        )
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_WRAP_T,
            GLES20.GL_CLAMP_TO_EDGE,
        )
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_MIN_FILTER,
            GLES20.GL_LINEAR,
        )
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES20.GL_TEXTURE_MAG_FILTER,
            GLES20.GL_LINEAR,
        )

        programHandle = buildProgram()
        positionAttribute = GLES20.glGetAttribLocation(programHandle, "a_Position")
        texCoordAttribute = GLES20.glGetAttribLocation(programHandle, "a_TexCoord")
        textureUniform = GLES20.glGetUniformLocation(programHandle, "u_Texture")
    }

    /**
     * Update the texture coordinates for the current display geometry.
     *
     * Only needs doing when ARCore says the geometry changed, which is on the first frame after
     * a resume or a rotation.
     */
    fun updateGeometry(frame: Frame) {
        if (!frame.hasDisplayGeometryChanged() && textureCoords.position() != 0) return
        quadCoords.rewind()
        textureCoords.rewind()
        frame.transformCoordinates2d(
            Coordinates2d.OPENGL_NORMALIZED_DEVICE_COORDINATES,
            quadCoords,
            Coordinates2d.TEXTURE_NORMALIZED,
            textureCoords,
        )
        quadCoords.rewind()
        textureCoords.rewind()
    }

    /** Draw the camera image. Call on the GL thread after [updateGeometry]. */
    fun draw() {
        if (programHandle == 0) return
        quadCoords.position(0)
        textureCoords.position(0)

        // The background covers every pixel, so depth testing and writing are pure overhead;
        // leaving depth writes on would also break anything drawn into the same buffer later.
        GLES20.glDisable(GLES20.GL_DEPTH_TEST)
        GLES20.glDepthMask(false)

        GLES20.glUseProgram(programHandle)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureUniform, 0)

        GLES20.glVertexAttribPointer(
            positionAttribute, 2, GLES20.GL_FLOAT, false, 0, quadCoords,
        )
        GLES20.glVertexAttribPointer(
            texCoordAttribute, 2, GLES20.GL_FLOAT, false, 0, textureCoords,
        )
        GLES20.glEnableVertexAttribArray(positionAttribute)
        GLES20.glEnableVertexAttribArray(texCoordAttribute)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, QUAD_VERTICES)

        GLES20.glDisableVertexAttribArray(positionAttribute)
        GLES20.glDisableVertexAttribArray(texCoordAttribute)
        GLES20.glDepthMask(true)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
    }

    private fun buildProgram(): Int {
        val vertex = compile(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER)
        val fragment = compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER)
        val program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, vertex)
        GLES20.glAttachShader(program, fragment)
        GLES20.glLinkProgram(program)

        val status = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            val log = GLES20.glGetProgramInfoLog(program)
            GLES20.glDeleteProgram(program)
            error("camera background program failed to link: $log")
        }
        // The shaders are linked into the program and are not needed again.
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)
        return program
    }

    private fun compile(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(shader)
            GLES20.glDeleteShader(shader)
            error("camera background shader failed to compile: $log")
        }
        return shader
    }

    private fun floatBuffer(values: FloatArray): FloatBuffer =
        ByteBuffer.allocateDirect(values.size * Float.SIZE_BYTES)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply {
                put(values)
                rewind()
            }

    private companion object {
        const val QUAD_VERTICES = 4

        val VERTEX_SHADER = """
            attribute vec4 a_Position;
            attribute vec2 a_TexCoord;
            varying vec2 v_TexCoord;
            void main() {
                gl_Position = a_Position;
                v_TexCoord = a_TexCoord;
            }
        """.trimIndent()

        // samplerExternalOES, not sampler2D: the camera image is a YUV texture the driver
        // converts on sample, and binding it to a plain sampler2D silently yields black.
        val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 v_TexCoord;
            uniform samplerExternalOES u_Texture;
            void main() {
                gl_FragColor = texture2D(u_Texture, v_TexCoord);
            }
        """.trimIndent()
    }
}
