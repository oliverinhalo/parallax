package uk.co.jacoblevy.parallax.diagnostics

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import uk.co.jacoblevy.parallax.R

/**
 * The screen a crash lands on.
 *
 * Deliberately built from platform views in code, with its colours written out here: it has to
 * render when the application's own graph, theme or resources are what failed, so it depends on
 * none of them. The same reason it is a plain `Activity` rather than a Compose one.
 */
class CrashActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val report = intent?.getStringExtra(EXTRA_REPORT)
            ?: CrashReporter.lastReport(this)
            ?: getString(android.R.string.unknownName)

        setContentView(screen(report))
        CrashReporter.clear(this)
    }

    private fun screen(report: String): ScrollView {
        val pad = dp(PADDING_DP)

        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(BACKGROUND)
        }

        column.addView(
            TextView(this).apply {
                text = getString(R.string.crash_title)
                setTextColor(ACCENT)
                setTypeface(Typeface.DEFAULT_BOLD)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, TITLE_SP)
            },
        )
        column.addView(
            TextView(this).apply {
                text = getString(R.string.crash_explanation)
                setTextColor(MUTED)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, BODY_SP)
                setPadding(0, dp(GAP_DP), 0, dp(GAP_DP))
            },
        )
        column.addView(
            LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.START
                addView(
                    Button(this@CrashActivity).apply {
                        text = getString(R.string.crash_share)
                        setOnClickListener { share(report) }
                    },
                )
            },
        )
        column.addView(
            TextView(this).apply {
                text = report
                setTextColor(FOREGROUND)
                setTypeface(Typeface.MONOSPACE)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, REPORT_SP)
                setTextIsSelectable(true)
                setPadding(0, dp(GAP_DP), 0, 0)
            },
        )

        return ScrollView(this).apply {
            setBackgroundColor(BACKGROUND)
            // Horizontal scrolling too: a stack frame is long and wrapping it makes the shape
            // of the trace -- which is most of what you read it for -- impossible to follow.
            addView(
                HorizontalScrollView(this@CrashActivity).apply {
                    addView(column, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
                },
                LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT),
            )
        }
    }

    private fun share(report: String) {
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, getString(R.string.crash_title))
            .putExtra(Intent.EXTRA_TEXT, report)
        startActivity(
            Intent.createChooser(send, getString(R.string.crash_share)),
        )
    }

    private fun dp(value: Float): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics)
            .toInt()

    companion object {
        const val EXTRA_REPORT = "uk.co.jacoblevy.parallax.CRASH_REPORT"

        private const val BACKGROUND = 0xFF0E1013.toInt()
        private const val FOREGROUND = 0xFFE6E9EE.toInt()
        private const val MUTED = 0xFF9AA3AF.toInt()
        private const val ACCENT = 0xFF3DE0FF.toInt()

        private const val PADDING_DP = 20f
        private const val GAP_DP = 12f
        private const val TITLE_SP = 20f
        private const val BODY_SP = 14f
        private const val REPORT_SP = 11f
    }
}
