package me.phie.tawc.compositor

import android.app.Activity
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.util.TypedValue
import android.view.Gravity
import android.view.ContextThemeWrapper
import android.view.View
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.WindowCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.R
import me.phie.tawc.launcher.IconLoader
import me.phie.tawc.launcher.Launch
import me.phie.tawc.launcher.LaunchState
import me.phie.tawc.ui.GraphicsOverride
import me.phie.tawc.ui.tonalButton

/**
 * The launch splash over a [CompositorActivity]'s SurfaceView: the entry's
 * icon and name while the program starts, its output log with a Close
 * button if it fails, gone once its window renders. A program that
 * exited or couldn't start also gets Relaunch, under the entry's graphics
 * override ([GraphicsOverride], as in the editor). It comes back if that
 * window closes again at once ([LaunchState.Closing]). A plain Android view,
 * so the compositor never draws it. White on light mode, black on dark;
 * [backdrop] (behind the system bars) matches it while the splash is up.
 * See notes/launcher.md ("Launch splash").
 */
internal class LaunchSplash(
    private val activity: Activity,
    private val backdrop: ViewGroup,
    private val launch: Launch,
    private val close: () -> Unit,
    private val relaunch: (GraphicsBackend?) -> Unit,
) {
    val view: FrameLayout
    private val splash: LinearLayout
    private val icon: ImageView
    private val logPanel: LinearLayout
    private val status: TextView
    private val logScroll: ScrollView
    private val logText: TextView
    private val relaunchControls: LinearLayout
    private val relaunchButton: View

    /** The overlay still covers the window (Back closes the task). */
    var active = true
        private set

    private val light = (activity.resources.configuration.uiMode and
        Configuration.UI_MODE_NIGHT_MASK) != Configuration.UI_MODE_NIGHT_YES
    private val bgColor = if (light) Color.WHITE else Color.BLACK

    init {
        val density = activity.resources.displayMetrics.density
        fun dp(v: Int) = (v * density).toInt()

        icon = ImageView(activity).apply {
            setImageResource(if (launch.terminal) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback)
        }
        val name = TextView(activity).apply {
            text = launch.name
            setTextColor(if (light) Color.BLACK else Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            gravity = Gravity.CENTER
            setPadding(dp(24), dp(16), dp(24), 0)
        }
        splash = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            addView(icon, LinearLayout.LayoutParams(dp(ICON_DP), dp(ICON_DP)))
            addView(name, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }

        status = TextView(activity).apply {
            setTextColor(if (light) Color.BLACK else Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, 0, 0, dp(12))
        }
        logText = TextView(activity).apply {
            setTextColor(if (light) 0xFF333333.toInt() else 0xFFCCCCCC.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(activity).apply {
            addView(logText, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        // The compositor theme isn't Material; the widgets need it.
        val themed = ContextThemeWrapper(activity, R.style.Theme_Tawc)
        val graphics = GraphicsOverride(themed, launch.graphics)
        relaunchControls = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            setPadding(0, dp(12), 0, 0)
            addView(TextView(themed).apply {
                text = activity.getString(R.string.launch_splash_cpu_hint)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(graphics.view, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        relaunchButton = themed.tonalButton(activity.getString(R.string.launch_splash_relaunch)) {
            relaunch(graphics.value)
        }.apply { visibility = View.GONE }
        val buttons = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
            addView(relaunchButton, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginEnd = dp(8) })
            addView(themed.tonalButton(activity.getString(R.string.launch_splash_close)) { close() })
        }
        logPanel = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
            visibility = View.GONE
            addView(status, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(relaunchControls, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(buttons, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(12) })
        }

        view = FrameLayout(activity).apply {
            setBackgroundColor(bgColor)
            // Nothing under the splash takes input until the window shows.
            isClickable = true
            addView(splash, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            addView(logPanel, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
        if (launch.state.value == LaunchState.Shown) {
            // Recreated over a live window: stay hidden until needed.
            active = false
            view.visibility = View.GONE
            setBars(Color.BLACK, false)
        } else {
            setBars(bgColor, light)
        }
    }

    private fun setBars(color: Int, lightIcons: Boolean) {
        backdrop.setBackgroundColor(color)
        // 3-button nav's scrim stays dark otherwise.
        activity.window.isNavigationBarContrastEnforced = !lightIcons
        WindowCompat.getInsetsController(activity.window, activity.window.decorView).apply {
            isAppearanceLightStatusBars = lightIcons
            isAppearanceLightNavigationBars = lightIcons
        }
    }

    /** Follow the launch until the window shows or the task closes. */
    fun bind(scope: CoroutineScope) {
        val targetPx = (ICON_DP * activity.resources.displayMetrics.density).toInt()
        if (launch.iconPath.isNotEmpty()) scope.launch {
            withContext(Dispatchers.IO) { IconLoader.decode(launch.iconPath, targetPx) }
                ?.let { icon.setImageBitmap(it) }
        }
        scope.launch { launch.state.collect(::render) }
        scope.launch {
            launch.logVersion.collect {
                if (logPanel.visibility == View.VISIBLE) showLog()
                // Coalesce bursts of output into a few redraws a second.
                delay(LOG_REFRESH_MS)
            }
        }
    }

    private fun render(state: LaunchState) {
        val failure = when (state) {
            LaunchState.TimedOut -> activity.getString(R.string.launch_splash_no_window, launch.name)
            is LaunchState.Exited -> activity.getString(R.string.launch_splash_exited, launch.name, state.code)
            is LaunchState.Failed -> activity.getString(R.string.launch_splash_failed, launch.name, state.message)
            else -> null
        }
        when {
            failure != null -> {
                show()
                status.text = failure
                // A timed-out program still runs; relaunching would double it.
                val canRelaunch = state != LaunchState.TimedOut
                relaunchControls.visibility = if (canRelaunch) View.VISIBLE else View.GONE
                relaunchButton.visibility = if (canRelaunch) View.VISIBLE else View.GONE
                splash.visibility = View.GONE
                logPanel.visibility = View.VISIBLE
                showLog()
            }
            state == LaunchState.Shown -> hide()
            state == LaunchState.Quit -> {
                Toast.makeText(
                    activity,
                    activity.getString(R.string.launch_splash_quit, launch.name),
                    Toast.LENGTH_LONG,
                ).show()
                close()
            }
            state == LaunchState.Detached || state == LaunchState.Closed -> close()
            state == LaunchState.Closing -> show()
        }
    }

    private fun showLog() {
        val atEnd = !logScroll.canScrollVertically(1)
        logText.text = launch.logText().ifEmpty { activity.getString(R.string.launch_splash_no_output) }
        if (atEnd) logScroll.post { logScroll.scrollTo(0, logText.height) }
    }

    private fun show() {
        if (active) return
        active = true
        setBars(bgColor, light)
        view.animate().cancel()
        view.alpha = 1f
        view.visibility = View.VISIBLE
    }

    private fun hide() {
        if (!active) return
        active = false
        setBars(Color.BLACK, false)
        view.animate().alpha(0f).setDuration(FADE_MS).withEndAction { view.visibility = View.GONE }
    }

    private companion object {
        const val ICON_DP = 96
        const val FADE_MS = 150L
        const val LOG_REFRESH_MS = 200L
    }
}
