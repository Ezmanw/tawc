package me.phie.tawc.launcher

import android.content.DialogInterface
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.content.res.AppCompatResources
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.PointerEmulation
import me.phie.tawc.R
import me.phie.tawc.Settings
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ui.Scaffold
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.graphicsBackendGroup
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.pointerEmulationGroup
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp
import java.io.File

/**
 * Edit a launcher entry's TAWC-side fields, or make a shortcut. Writes
 * only ever go to the [LauncherStore]; no `.desktop` file is touched.
 *
 * - A scanned entry shows each field's effective value. A field that
 *   differs from the scanned (packaged) one becomes an override, with a
 *   reset control beside it; a blank field falls back to the packaged
 *   value, shown as its hint. Toolbar Reset drops every override but
 *   hide state.
 * - A shortcut is edited from its store record with the same form,
 *   plus toolbar Delete (which also retires its home-screen pin). Add
 *   entry makes a new one.
 *
 * The widgets are read into an [EntryForm], which is the dirty check,
 * the reset controls' state and what Save writes. Form: Command
 * (required) + Name (blank = the command), environment variables
 * (exported at launch), Icon (a theme name or in-rootfs path with a live
 * preview, picked by [IconPickerActivity], or an image imported by
 * [IconImport] into the store), Terminal, and graphics backend /
 * pointer emulation overrides. Launched for result by [AppsPane];
 * RESULT_OK = rescan. Rotation is a handled config change (manifest),
 * so the form survives it.
 */
class EntryEditorActivity : AppCompatActivity() {

    /** What the form edits. */
    private sealed interface Target {
        /** A scanned entry: the fields become overrides of its packaged values. */
        class Scanned(val entry: LauncherEntry) : Target

        /** A store shortcut, or a new one when [id] is null. */
        class Shortcut(val id: String?, val fields: EntryFields) : Target
    }

    private val launcher by lazy { LauncherStore(this) }

    private lateinit var scaffold: Scaffold
    private lateinit var rootfs: File
    private var installId = ""

    private var target: Target? = null
    private val scannedEntry: LauncherEntry? get() = (target as? Target.Scanned)?.entry

    private lateinit var nameField: EditText
    private lateinit var execField: EditText
    private lateinit var iconField: EditText
    private lateinit var iconPreview: ImageView
    private lateinit var terminalCheckbox: CheckBox
    private lateinit var graphicsOverride: CheckBox
    private lateinit var pointerOverride: CheckBox
    private lateinit var envRows: LinearLayout
    private val envFields = ArrayList<Pair<EditText, EditText>>()
    private lateinit var saveButton: MaterialButton

    /** Per-field reset (scanned) / clear-image (shortcut icon) controls. */
    private val resets = HashMap<String, View>()

    /** Icon state not held by a widget: the store file in effect and an
     *  unsaved Load. The icon field is blank while either is set. */
    private var iconFile: String? = null
    private var pendingIcon: IconImport.Result? = null

    /** The radio picks, meaningful while the override box is checked. */
    private var graphicsChoice: GraphicsBackend = GraphicsBackend.DEFAULT
    private var pointerChoice: PointerEmulation = PointerEmulation.DEFAULT

    /** The form as loaded; the dirty check compares against it. */
    private var initial: EntryForm? = null

    private val iconLoader by lazy {
        IconLoader(lifecycleScope, (PREVIEW_DP * resources.displayMetrics.density).toInt())
    }
    private var previewJob: Job? = null

    private val pickIcon = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        result.data?.getStringExtra(IconPickerActivity.EXTRA_NAME)
            ?.takeIf { result.resultCode == RESULT_OK }
            ?.let { setIcon(it) }
    }

    private val loadIcon = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { importIcon(it) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installId = intent?.getStringExtra(EXTRA_ID) ?: ""
        val inst = InstallationStore(this).load(installId) ?: return finish()
        rootfs = launcher.rootfs(installId)
        val entryId = intent?.getStringExtra(EXTRA_ENTRY)
        // The screen goes up at once (a later setContentView misses the
        // system-bar insets); the form fills in once the entry loads.
        scaffold = buildChildScreen(getString(if (entryId == null) R.string.editor_title_new else R.string.editor_title_edit))
        // ✕, not ←: leaving discards the form, so say so (Material's
        // full-screen dialog pattern). Unsaved changes confirm first.
        scaffold.toolbar.apply {
            navigationIcon = AppCompatResources.getDrawable(this@EntryEditorActivity, R.drawable.ic_close)
                ?.mutate()
                ?.apply { setTintList(controlTint()) }
            navigationContentDescription = getString(R.string.editor_close)
            setNavigationOnClickListener { requestClose() }
        }
        onBackPressedDispatcher.addCallback(this, discardGuard)
        setContentView(scaffold.root)
        lifecycleScope.launch {
            val loaded = withContext(Dispatchers.IO) { loadTarget(inst, entryId) }
            if (loaded == null) {
                Toast.makeText(this@EntryEditorActivity, R.string.shortcut_entry_gone, Toast.LENGTH_LONG).show()
                finish()
                return@launch
            }
            target = loaded
            buildForm(loaded)
        }
    }

    /** [entryId]'s target: a store shortcut by its key, else the merged
     *  scanned entry; null when gone. No id = a new shortcut. */
    private fun loadTarget(inst: Installation, entryId: String?): Target? {
        if (entryId == null) return Target.Shortcut(null, EntryFields())
        launcher.load(installId)?.shortcuts?.get(entryId)?.let { return Target.Shortcut(entryId, it) }
        return LauncherEntry.find(applicationContext, inst, entryId)
            ?.takeIf { it.source == LauncherEntry.Source.DESKTOP }
            ?.let { Target.Scanned(it) }
    }

    private fun buildForm(t: Target) {
        val initial = when (t) {
            is Target.Scanned -> EntryForm.of(t.entry)
            is Target.Shortcut -> EntryForm.of(t.fields, terminalDefault = t.id == null)
        }
        iconFile = initial.iconFile
        val pad = (16 * resources.displayMetrics.density).toInt()

        val form = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        execField = addField(form, "exec", R.string.editor_field_exec, initial.exec, pad)
        nameField = addField(form, "name", R.string.editor_field_name, initial.name, pad)
        addEnvSection(form, initial.env, pad)
        iconField = addIconRow(form, initial.icon, pad)
        val terminalRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        terminalCheckbox = CheckBox(this).apply {
            text = getString(R.string.editor_field_terminal)
            isChecked = initial.terminal
            // The fallback glyph follows Terminal.
            setOnCheckedChangeListener { _, _ -> refreshPreview(debounce = false); revalidate() }
        }
        terminalRow.addView(terminalCheckbox, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(terminalRow, "terminal") { terminalCheckbox.isChecked = scannedEntry!!.packagedTerminal }
        form.addView(terminalRow, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        // Unchecked = the global setting; checking starts from it.
        graphicsChoice = initial.graphics ?: Settings.graphicsBackend
        graphicsOverride = addOverrideBlock(
            form, R.string.editor_override_graphics, initial.graphics != null, pad / 2,
            graphicsBackendGroup(graphicsChoice) { graphicsChoice = it; revalidate() },
        )
        pointerChoice = initial.pointer ?: Settings.pointerEmulation
        pointerOverride = addOverrideBlock(
            form, R.string.editor_override_pointer_emulation, initial.pointer != null, pad,
            pointerEmulationGroup(pointerChoice) { pointerChoice = it; revalidate() },
        )

        saveButton = primaryButton(getString(R.string.editor_save)) { save() }
        form.addView(saveButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        // Delete/Reset live in the toolbar, not as a big destructive
        // button next to Save; both confirm. Tinted manually —
        // MaterialToolbar doesn't tint menu-item icons, and the vectors'
        // own fill is white.
        val reset = t is Target.Scanned && t.entry.overridden.isNotEmpty()
        if (reset || (t is Target.Shortcut && t.id != null)) {
            scaffold.toolbar.menu.add(getString(if (reset) R.string.editor_reset else R.string.editor_delete)).apply {
                icon = AppCompatResources
                    .getDrawable(this@EntryEditorActivity, if (reset) R.drawable.ic_reset else R.drawable.ic_delete)
                    ?.mutate()
                    ?.apply { setTint(getColor(R.color.tawc_danger)) }
                setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS)
                setOnMenuItemClickListener { confirmRemove(reset); true }
            }
        }

        scaffold.content.addView(
            ScrollView(this).apply { addView(form) },
            LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT),
        )
        this.initial = initial
        revalidate()
        refreshPreview(debounce = false)
    }

    /** The widgets' current values. */
    private fun form() = EntryForm(
        exec = execField.text.toString(),
        name = nameField.text.toString(),
        icon = iconField.text.toString(),
        iconFile = iconFile,
        pendingIcon = pendingIcon,
        terminal = terminalCheckbox.isChecked,
        env = envFields.map { (k, v) -> k.text.toString().trim() to v.text.toString() }
            .filter { (k, v) -> k.isNotEmpty() || v.isNotEmpty() },
        graphics = graphicsChoice.takeIf { graphicsOverride.isChecked },
        pointer = pointerChoice.takeIf { pointerOverride.isChecked },
    )

    private fun isDirty() = initial.let { it != null && it != form() }

    /** Checkbox + [group] (shown while checked) as one block, so its
     *  bottom margin holds with the group hidden. Returns the checkbox. */
    private fun addOverrideBlock(form: LinearLayout, labelRes: Int, checked: Boolean, margin: Int, group: View): CheckBox {
        group.visibility = if (checked) View.VISIBLE else View.GONE
        val box = CheckBox(this).apply {
            text = getString(labelRes)
            isChecked = checked
            setOnCheckedChangeListener { _, on ->
                group.visibility = if (on) View.VISIBLE else View.GONE
                revalidate()
            }
        }
        val block = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        block.addView(box, verticalLp(WRAP_CONTENT, WRAP_CONTENT))
        block.addView(group, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        form.addView(block, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = margin))
        return box
    }

    /** A reset control for [field] at the end of [row], shown by
     *  [revalidate] while the field differs from the packaged value.
     *  Shortcuts get one for the icon only, clearing an image. */
    private fun addReset(row: LinearLayout, field: String, onClick: () -> Unit) {
        val scanned = target is Target.Scanned
        val clearImage = field == "icon" && !scanned
        if (!scanned && !clearImage) return
        val btn = plainIconButton(
            if (clearImage) R.drawable.ic_close else R.drawable.ic_reset,
            getString(if (clearImage) R.string.editor_icon_clear else R.string.editor_field_reset),
        ) { onClick(); revalidate() }
        btn.visibility = View.GONE
        resets[field] = btn
        row.addView(btn, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
    }

    private fun label(form: LinearLayout, labelRes: Int) {
        form.addView(
            TextView(this).apply { text = getString(labelRes); textSize = 14f },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
    }

    /** Label + single-line EditText with a reset control beside it. A
     *  scanned entry's packaged value is the hint. */
    private fun addField(form: LinearLayout, field: String, labelRes: Int, value: String, pad: Int): EditText {
        label(form, labelRes)
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val edit = EditText(this).apply {
            setText(value)
            isSingleLine = true
            scannedEntry?.let { hint = it.packagedValue(field) }
            doAfterTextChanged { revalidate() }
        }
        row.addView(edit, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(row, field) { edit.setText(scannedEntry!!.packagedValue(field)) }
        form.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        return edit
    }

    /**
     * ```
     * Icon
     * [preview]  [ field ............ ✕ ] ↺
     *            [ Select ]  [ Load ]
     * ```
     */
    private fun addIconRow(form: LinearLayout, value: String, pad: Int): EditText {
        label(form, R.string.editor_field_icon)
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.TOP
        }
        val previewPx = (PREVIEW_DP * resources.displayMetrics.density).toInt()
        iconPreview = ImageView(this).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
        row.addView(iconPreview, LinearLayout.LayoutParams(previewPx, previewPx).also {
            it.marginEnd = pad
            it.topMargin = pad / 4
        })

        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val field = TextInputEditText(this).apply {
            setText(value)
            isSingleLine = true
            // Icon names aren't words; no spellcheck squiggles.
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            // As tall as the ✕, so the row doesn't jump when it shows.
            minHeight = (48 * resources.displayMetrics.density).toInt()
            doAfterTextChanged {
                // A name replaces an image.
                if (!it.isNullOrEmpty()) clearIconImage()
                revalidate()
                refreshPreview(debounce = true)
            }
        }
        // Box-less, so it reads like the plain fields above; the layout
        // is only here for the clear (✕) end icon.
        val layout = TextInputLayout(this).apply {
            boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_NONE
            isHintEnabled = false
            endIconMode = TextInputLayout.END_ICON_CLEAR_TEXT
            addView(field)
        }
        val fieldRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        fieldRow.addView(layout, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(fieldRow, "icon") {
            clearIconImage()
            setIcon(scannedEntry?.packagedValue("icon").orEmpty())
        }
        column.addView(fieldRow, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.addView(tonalButton(getString(R.string.editor_icon_select)) {
            pickIcon.launch(Intent(this, IconPickerActivity::class.java).putExtra(IconPickerActivity.EXTRA_ID, installId))
        }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).also { it.marginEnd = pad / 2 })
        buttons.addView(tonalButton(getString(R.string.editor_icon_load)) {
            loadIcon.launch(arrayOf("image/*"))
        }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        column.addView(buttons, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        row.addView(column, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        form.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        return field
    }

    private fun clearIconImage() {
        iconFile = null
        pendingIcon = null
    }

    /** Put [value] in the icon field (a non-empty one replaces an image). */
    private fun setIcon(value: String) {
        iconField.setText(value)
        iconField.setSelection(value.length)
        refreshPreview(debounce = false)
    }

    /** Show what the launcher grid would draw: the image in effect, else
     *  the resolved Icon value (blank on a scanned entry = packaged),
     *  else the same fallback glyph. */
    private fun refreshPreview(debounce: Boolean) {
        if (!::iconPreview.isInitialized || !::terminalCheckbox.isInitialized) return
        val fallback = if (terminalCheckbox.isChecked) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback
        val pending = pendingIcon
        val stored = iconFile?.let { File(launcher.iconsDir(installId), it).path }
        val value = iconField.text.toString().ifBlank { scannedEntry?.packagedValue("icon").orEmpty() }
        val rootfsPath = rootfs.absolutePath
        previewJob?.cancel()
        previewJob = lifecycleScope.launch {
            if (debounce) delay(PREVIEW_DEBOUNCE_MS)
            if (pending != null) {
                val bmp = withContext(Dispatchers.IO) { BitmapFactory.decodeByteArray(pending.bytes, 0, pending.bytes.size) }
                // A null tag makes an in-flight loader completion stale.
                iconPreview.tag = null
                if (bmp != null) iconPreview.setImageBitmap(bmp) else iconPreview.setImageResource(fallback)
                return@launch
            }
            val path = stored ?: if (value.isBlank()) "" else withContext(Dispatchers.IO) {
                runCatching { NativeBridge.nativeResolveIcon(rootfsPath, value) }.getOrDefault("")
            }
            iconLoader.load(path, iconPreview, fallback)
        }
    }

    /** Read a picked image as PNG ([IconImport]); it lands in the store
     *  on Save. Failure leaves the icon as it was. */
    private fun importIcon(uri: Uri) {
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { IconImport.import(this@EntryEditorActivity, uri, File(cacheDir, "icon-import")) }
            }
            result.onSuccess { r ->
                pendingIcon = r
                iconFile = null
                iconField.setText("")
                revalidate()
                refreshPreview(debounce = false)
            }.onFailure {
                Toast.makeText(
                    this@EntryEditorActivity,
                    getString(R.string.editor_icon_import_failed, it.message),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    /**
     * ```
     * Environment variables
     * [ NAME      ] [ value          ] ✕
     * [ + Add ]
     * ```
     */
    private fun addEnvSection(form: LinearLayout, env: List<Pair<String, String>>, pad: Int) {
        label(form, R.string.editor_field_env)
        envRows = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        form.addView(envRows, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        for ((k, v) in env) addEnvRow(k, v)
        form.addView(
            tonalButton(getString(R.string.editor_env_add)) { addEnvRow("", "").first.requestFocus() }.apply {
                icon = AppCompatResources.getDrawable(this@EntryEditorActivity, R.drawable.ic_add)
                iconTint = ColorStateList.valueOf(getColor(R.color.tawc_on_tonal))
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT, bottomMargin = pad),
        )
    }

    /** One `[name] [value] ✕` row; returns its two fields. */
    private fun addEnvRow(name: String, value: String): Pair<EditText, EditText> {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        fun field(text: String, hintRes: Int) = EditText(this).apply {
            setText(text)
            hint = getString(hintRes)
            isSingleLine = true
            // Variable names and values aren't words.
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            doAfterTextChanged { revalidate() }
        }
        val key = field(name, R.string.editor_env_name_hint)
        val value = field(value, R.string.editor_env_value_hint)
        row.addView(key, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        row.addView(value, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        row.addView(plainIconButton(R.drawable.ic_close, getString(R.string.editor_env_remove)) {
            envRows.removeView(row)
            envFields.removeAll { it.first === key }
            revalidate()
        })
        envRows.addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return (key to value).also { envFields += it }
    }

    /** Flag [form]'s bad env names on their rows; true when none. */
    private fun validateEnv(form: EntryForm): Boolean {
        val bad = form.badEnvNames()
        for ((k, _) in envFields) {
            val name = k.text.toString().trim()
            k.error = when {
                name !in bad -> null
                DesktopEntryFile.isValidEnvName(name) -> getString(R.string.editor_env_duplicate)
                else -> getString(R.string.editor_env_invalid)
            }
        }
        return bad.isEmpty()
    }

    /** Reset controls, hints, Save and the back guard from the form. */
    private fun revalidate() {
        if (!::saveButton.isInitialized) return
        val form = form()
        fun show(field: String, on: Boolean) {
            resets[field]?.visibility = if (on) View.VISIBLE else View.GONE
        }
        when (val t = target!!) {
            is Target.Scanned -> {
                // Same computation as Save: a control shows while the
                // field would be written as an override.
                val o = form.overrides(t.entry, EntryFields())
                show("name", o.name != null)
                show("exec", o.exec != null)
                show("icon", form.hasImage || o.icon != null)
                show("terminal", o.terminal != null)
                nameField.hint = t.entry.packagedValue("name")
                iconField.hint = if (form.hasImage) getString(R.string.editor_icon_imported) else t.entry.packagedValue("icon")
            }
            is Target.Shortcut -> {
                show("icon", form.hasImage)
                nameField.hint = form.exec.trim()
                iconField.hint = if (form.hasImage) getString(R.string.editor_icon_imported) else null
            }
        }
        val envOk = validateEnv(form)
        saveButton.isEnabled = (form.exec.isNotBlank() || target is Target.Scanned) && envOk
        discardGuard.isEnabled = isDirty()
    }

    /** Back/gesture interception, enabled only while there are unsaved
     *  changes so a clean form keeps the system's predictive back. */
    private val discardGuard = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = requestClose()
    }

    /** Close, confirming first when that would discard changes. */
    private fun requestClose() {
        if (!isDirty()) {
            finish()
            return
        }
        val dialog = AlertDialog.Builder(this)
            .setMessage(R.string.editor_discard_confirm)
            .setPositiveButton(R.string.editor_discard) { _, _ -> finish() }
            .setNegativeButton(R.string.editor_keep_editing, null)
            .show()
        neutralNegative(dialog)
    }

    /** Neutral negative button, as on the app's other confirms. */
    private fun neutralNegative(dialog: AlertDialog) {
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
            btn.setTextColor(MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant))
        }
    }

    private fun controlTint(): ColorStateList? = TypedValue().let {
        theme.resolveAttribute(androidx.appcompat.R.attr.colorControlNormal, it, true)
        if (it.resourceId != 0) getColorStateList(it.resourceId) else ColorStateList.valueOf(it.data)
    }

    /** Write the form to the store: an override of a scanned entry, the
     *  shortcut being edited, or a new `tawc:app:` shortcut. */
    private fun save() {
        val t = target ?: return
        val form = form()
        saveButton.isEnabled = false
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    launcher.update(installId, form.pendingIcon?.let { it.bytes to it.slug }) { store, newIcon ->
                        when (t) {
                            is Target.Scanned ->
                                store.withOverride(t.entry.id, form.overrides(t.entry, store.overrides[t.entry.id] ?: EntryFields(), newIcon))
                            is Target.Shortcut -> {
                                val id = t.id ?: store.newShortcutId(form.name.trim().ifEmpty { form.exec.trim() })
                                store.withShortcut(id, form.shortcut(store.shortcuts[id] ?: EntryFields(), newIcon))
                            }
                        }
                    }
                }
            }
            val written = result.getOrNull()
            if (written != null) {
                setResult(RESULT_OK)
                finish()
                return@launch
            }
            val message = result.exceptionOrNull()?.let { getString(R.string.editor_save_failed, it.message) }
                ?: getString(R.string.editor_delete_failed)
            Toast.makeText(this@EntryEditorActivity, message, Toast.LENGTH_LONG).show()
            revalidate()
        }
    }

    /** Reset (drop a scanned entry's overrides, keeping hide state) or
     *  Delete (a shortcut, retiring its pin). */
    private fun confirmRemove(reset: Boolean) {
        val id = when (val t = target) {
            is Target.Scanned -> t.entry.id
            is Target.Shortcut -> t.id ?: return
            null -> return
        }
        val label = nameField.text.toString().ifBlank { scannedEntry?.name?.ifBlank { null } ?: execField.text.toString().ifBlank { id } }
        val dialog = AlertDialog.Builder(this)
            .setMessage(getString(if (reset) R.string.editor_reset_confirm else R.string.editor_delete_confirm, label))
            .setPositiveButton(if (reset) R.string.editor_reset else R.string.editor_delete) { _, _ -> remove(id, reset) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        // Red Delete, as on the app's other confirms.
        if (!reset) dialog.getButton(DialogInterface.BUTTON_POSITIVE)?.setTextColor(getColor(R.color.tawc_danger))
        neutralNegative(dialog)
    }

    private fun remove(id: String, reset: Boolean) {
        lifecycleScope.launch {
            val written = withContext(Dispatchers.IO) {
                val w = runCatching {
                    launcher.update(installId) { store, _ -> if (reset) store.withoutOverrides(id) else store.withoutShortcut(id) }
                }.getOrNull()
                if (w != null && !reset) {
                    runCatching {
                        EntryShortcuts.disablePin(applicationContext, installId, id, getString(R.string.shortcut_entry_deleted))
                    }
                }
                w
            }
            if (written == null) {
                Toast.makeText(this@EntryEditorActivity, R.string.editor_delete_failed, Toast.LENGTH_LONG).show()
                return@launch
            }
            setResult(RESULT_OK)
            finish()
        }
    }

    companion object {
        /** Installation id. */
        const val EXTRA_ID = "id"

        /** [LauncherEntry.id] to edit; absent = new shortcut. */
        const val EXTRA_ENTRY = "entry"

        /** Icon preview edge, close to the launcher grid's icon. */
        private const val PREVIEW_DP = 48f

        private const val PREVIEW_DEBOUNCE_MS = 250L
    }
}
