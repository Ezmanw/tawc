package me.phie.tawc.ui

import android.content.Context
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.LinearLayout
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.R
import me.phie.tawc.Settings

/**
 * A per-entry override of a global setting: a checkbox over [group],
 * which shows while it's checked. One view, so a bottom margin on it
 * holds with the group hidden. [onToggle] runs after the visibility flip.
 */
class OverrideBlock(context: Context, label: String, checked: Boolean, group: View, onToggle: () -> Unit = {}) {
    val box = CheckBox(context).apply {
        text = label
        isChecked = checked
    }
    val view = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        addView(box, verticalLp(WRAP_CONTENT, WRAP_CONTENT))
        addView(group, verticalLp(MATCH_PARENT, WRAP_CONTENT))
    }

    init {
        group.visibility = if (checked) View.VISIBLE else View.GONE
        box.setOnCheckedChangeListener { _, on ->
            group.visibility = if (on) View.VISIBLE else View.GONE
            onToggle()
        }
    }
}

/**
 * An entry's graphics backend override, shared by the `.desktop` editor
 * and the launch splash's Relaunch: [OverrideBlock] over
 * [graphicsBackendGroup]. [initial] null = unchecked; checking starts
 * from the global setting. [onChange] runs on any change.
 */
class GraphicsOverride(context: Context, initial: GraphicsBackend?, onChange: () -> Unit = {}) {
    private var choice = initial ?: Settings.graphicsBackend
    private val block = OverrideBlock(
        context,
        context.getString(R.string.editor_override_graphics),
        initial != null,
        context.graphicsBackendGroup(choice) { choice = it; onChange() },
        onChange,
    )
    val view: View get() = block.view

    /** The override, or null while unchecked. */
    val value: GraphicsBackend? get() = choice.takeIf { block.box.isChecked }
}
