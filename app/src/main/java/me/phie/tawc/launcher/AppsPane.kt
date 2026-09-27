package me.phie.tawc.launcher

import android.content.Context
import android.content.Intent
import android.os.SystemClock
import android.view.Gravity
import android.view.KeyEvent
import android.view.Menu
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.doAfterTextChanged
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.paneTopRowHeightPx
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.verticalLp

/**
 * The home screen's apps pane: type-to-filter list of installed
 * `.desktop` apps for one distro. The Rust compositor library does the
 * actual scanning ([LauncherEntry.scan]); Kotlin here just renders +
 * filters + dispatches launches.
 *
 * UX is intentionally minimal: `[≡][Search <distro>][⋮]`, one scrolling
 * list, Enter launches the top match, tap launches that row. Long-press
 * opens a per-entry action menu (Hide/Unhide, Add to home screen, Edit
 * — assembled in [entryActionsFor]). The home ⋮ gets this pane's
 * items from [addMenuItems] (Show hidden, Add entry…). Pinning,
 * frecency, window-list integration are deferred (see
 * notes/launcher.md "Future UX").
 *
 * Launches are fire-and-forget via [EntryLauncher], whose process-wide
 * scope outlives the pane. The list is rescanned on every show and
 * resume, so packages installed from the terminal show up.
 *
 * Hidden entries ([Installation.hiddenDesktopIds]) are filtered here in
 * Kotlin, not in the Rust scanner — hide state is per-install app
 * metadata, and the scanner is shared with window icon/title resolution
 * which must keep seeing hidden apps (notes/launcher.md).
 */
internal class AppsPane(
    private val activity: AppCompatActivity,
    private var installation: Installation,
    private val host: Host,
) {

    interface Host {
        fun openDrawer()
        fun showMenu(anchor: View)
        /** Start the `.desktop` editor for result; RESULT_OK → [rescan]. */
        fun openEditor(intent: Intent)
    }

    private val store = InstallationStore(activity)
    private val density = activity.resources.displayMetrics.density
    private val pad = (16 * density).toInt()

    private val searchField: EditText
    private val listColumn: LinearLayout
    private val emptyView: TextView

    /** Full app list (from the last scan). Filtered subset is rebuilt on every keystroke. */
    private var allEntries: List<LauncherEntry> = emptyList()
    private var filteredEntries: List<LauncherEntry> = emptyList()

    /** Render hidden entries (dimmed, in sort position). Transient
     *  per-pane state, deliberately not persisted. */
    private var showHidden = false

    /** UI scope for loading + search filtering. Cancelled on [destroy]. */
    private val uiScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private val iconSizePx = (ICON_SIZE_DP * density).toInt()
    private val iconLoader = IconLoader(uiScope, iconSizePx)

    /** A hardware Enter arrives both as a key event and as the IME
     *  editor action (~10ms apart); without a debounce one press
     *  launches twice. */
    private var lastLaunchMs = 0L

    val view: LinearLayout

    init {
        view = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }

        searchField = EditText(activity).apply {
            hint = activity.getString(R.string.hint_search_distro, DistroRegistry.displayLabel(installation))
            textSize = 18f
            isSingleLine = true
            imeOptions = EditorInfo.IME_ACTION_GO
            isFocusableInTouchMode = true
            doAfterTextChanged { applyFilter() }
            setOnEditorActionListener { _, actionId, event ->
                val isEnter = actionId == EditorInfo.IME_ACTION_GO ||
                    actionId == EditorInfo.IME_ACTION_DONE ||
                    (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
                if (isEnter) { launchTop(); true } else false
            }
        }
        val drawerButton = activity.plainIconButton(
            R.drawable.ic_menu,
            activity.getString(R.string.action_open_drawer),
        ) { host.openDrawer() }
        // Under the shared glyph size: three solid dots read heavier
        // than the line icons everything else uses.
        lateinit var menuButton: View
        menuButton = activity.plainIconButton(
            R.drawable.ic_more_vert,
            activity.getString(R.string.home_menu_description),
            iconSizeDp = 21,
        ) { host.showMenu(menuButton) }
        val searchRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(pad / 4, 0, pad / 4, 0)
        }
        val button = activity.tawcButtonSizePx()
        searchRow.addView(
            drawerButton,
            LinearLayout.LayoutParams(button, button).also { it.marginEnd = pad / 4 },
        )
        searchRow.addView(searchField, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        searchRow.addView(
            menuButton,
            LinearLayout.LayoutParams(button, button).also { it.marginStart = pad / 4 },
        )
        view.addView(searchRow, LinearLayout.LayoutParams(MATCH_PARENT, activity.paneTopRowHeightPx()))

        emptyView = TextView(activity).apply {
            text = activity.getString(R.string.launcher_loading_apps)
            textSize = 14f
            alpha = 0.7f
            setPadding(pad, pad / 2, pad, 0)
        }
        view.addView(emptyView, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))

        listColumn = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, pad * 5)
        }
        val scroll = ScrollView(activity).apply {
            setFillViewport(true)
            clipToPadding = false
            isVerticalFadingEdgeEnabled = true
            setFadingEdgeLength(pad)
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(listColumn)
        }
        view.addView(scroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        rescan()
    }

    fun onResume() {
        // Hide state may have changed elsewhere (another pane instance).
        store.load(installation.id)?.let { installation = it }
        rescan()
    }

    fun destroy() {
        uiScope.cancel()
    }

    fun showSoftKeyboard() {
        searchField.requestFocus()
        // Post: on first show the field isn't attached to the window yet.
        searchField.post {
            val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
            imm?.showSoftInput(searchField, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    /** This pane's group of the home ⋮ menu. */
    fun addMenuItems(menu: Menu, order: Int) {
        val hidden = hiddenCount()
        if (hidden > 0) {
            menu.add(Menu.NONE, Menu.NONE, order, activity.getString(R.string.launcher_menu_show_hidden, hidden))
                .apply {
                isCheckable = true
                isChecked = showHidden
                setOnMenuItemClickListener {
                    showHidden = !showHidden
                    applyFilter()
                    true
                }
            }
        }
        if (canEditEntries()) {
            menu.add(Menu.NONE, Menu.NONE, order, R.string.launcher_menu_add_entry).setOnMenuItemClickListener {
                openEditor(null)
                true
            }
        }
    }

    fun rescan() {
        val rootfs = store.rootfsDir(installation.id).absolutePath
        uiScope.launch {
            allEntries = withContext(Dispatchers.IO) { LauncherEntry.scan(rootfs) }
            applyFilter()
            if (allEntries.isEmpty()) {
                emptyView.text = activity.getString(R.string.launcher_no_launchable_apps)
            }
        }
    }

    /** Ids the user hid, from the current metadata record. */
    private fun hiddenIds(): Set<String> = installation.hiddenDesktopIds.toSet()

    /** Hidden entries that actually exist in this rootfs (stale ids don't count). */
    private fun hiddenCount(): Int {
        val hidden = hiddenIds()
        return allEntries.count { it.id in hidden }
    }

    /** Re-filter [allEntries] against hide state + the search field
     *  ([LauncherEntry.filter]) and re-render. */
    private fun applyFilter() {
        filteredEntries = LauncherEntry.filter(
            allEntries, hiddenIds(), showHidden, searchField.text.toString(),
        )
        renderList()
    }

    private fun renderList() {
        listColumn.removeAllViews()
        if (filteredEntries.isEmpty() && allEntries.isNotEmpty()) {
            val q = searchField.text.toString().trim()
            emptyView.text = if (q.isEmpty()) {
                // Every entry is hidden (show-hidden off): keep the
                // no-apps message but say why the list is empty.
                activity.getString(R.string.launcher_no_launchable_apps) + "\n" +
                    activity.getString(R.string.launcher_hidden_count_hint, hiddenCount())
            } else {
                activity.getString(R.string.launcher_no_matches)
            }
            emptyView.visibility = View.VISIBLE
            return
        }
        emptyView.visibility = if (allEntries.isEmpty()) View.VISIBLE else View.GONE
        val hidden = hiddenIds()
        val rowPad = (12 * density).toInt()
        val rowMargin = (6 * density).toInt()
        for (entry in filteredEntries) {
            val row = buildRow(entry, rowPad, dimmed = entry.id in hidden)
            listColumn.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = rowMargin))
        }
    }

    /**
     * Editor writes are plain app-uid file I/O into the rootfs — works
     * for tawcroot/proot but not chroot's root-owned rootfs
     * (notes/launcher.md "Access model"), so chroot installs get no
     * New/Edit entry points, consistent with the terminal gating.
     */
    private fun canEditEntries(): Boolean = installation.method != Installation.METHOD_CHROOT

    private fun openEditor(entryPath: String?) {
        val i = Intent(activity, DesktopFileEditorActivity::class.java)
            .putExtra(DesktopFileEditorActivity.EXTRA_ID, installation.id)
        if (entryPath != null) i.putExtra(DesktopFileEditorActivity.EXTRA_PATH, entryPath)
        host.openEditor(i)
    }

    /**
     * One long-press menu item. Assembled per entry by [entryActionsFor];
     * conditional items are dropped there (`takeIf`/`listOfNotNull`).
     */
    private data class EntryAction(
        val label: CharSequence,
        val run: () -> Unit,
    )

    private fun entryActionsFor(entry: LauncherEntry): List<EntryAction> {
        val hidden = entry.id in hiddenIds()
        return listOfNotNull(
            if (hidden) {
                EntryAction(activity.getString(R.string.launcher_action_unhide)) { setEntryHidden(entry, false) }
            } else {
                EntryAction(activity.getString(R.string.launcher_action_hide)) { setEntryHidden(entry, true) }
            },
            EntryAction(activity.getString(R.string.launcher_action_add_home)) { pinEntry(entry) },
            // Only entries in the managed dir are editable — everything
            // else is package-owned (see DesktopEntryFile).
            EntryAction(activity.getString(R.string.launcher_action_edit)) { openEditor(entry.path) }
                .takeIf {
                    canEditEntries() &&
                        DesktopEntryFile.isManaged(entry.path, store.rootfsDir(installation.id))
                },
        )
    }

    private fun showEntryMenu(entry: LauncherEntry) {
        val actions = entryActionsFor(entry)
        if (actions.isEmpty()) return
        AlertDialog.Builder(activity)
            .setTitle(entry.name.ifEmpty { entry.id })
            .setItems(actions.map { it.label }.toTypedArray()) { _, which ->
                actions[which].run()
            }
            .show()
    }

    /**
     * Pin [entry] to the home screen ([EntryShortcuts]). Icon decode is
     * I/O, so build the request off the main thread; the system pin
     * sheet takes over from there.
     */
    private fun pinEntry(entry: LauncherEntry) {
        val inst = installation
        uiScope.launch {
            val result = withContext(Dispatchers.IO) {
                EntryShortcuts.requestPin(activity, inst, entry)
            }
            val toast = when (result) {
                EntryShortcuts.PinResult.REQUESTED -> null
                EntryShortcuts.PinResult.UPDATED -> R.string.shortcut_pin_updated
                EntryShortcuts.PinResult.UNSUPPORTED -> R.string.shortcut_pin_unsupported
            }
            toast?.let { Toast.makeText(activity, it, Toast.LENGTH_SHORT).show() }
        }
    }

    /**
     * Persist hide/unhide through the locked read-modify-write.
     * [InstallationStore.update] returns the record it wrote, which
     * becomes the new [installation] so the filter sees the fresh set;
     * null (lost race against uninstall) just leaves the list as-is —
     * the whole slot is going away.
     */
    private fun setEntryHidden(entry: LauncherEntry, hidden: Boolean) {
        store.update(installation.id) { it.withEntryHidden(entry.id, hidden) }
            ?.let { installation = it }
        applyFilter()
    }

    private fun buildRow(entry: LauncherEntry, rowPad: Int, dimmed: Boolean = false): View {
        val card = activity.tawcCard().apply {
            isClickable = true
            isFocusable = true
            isLongClickable = true
            if (dimmed) alpha = 0.5f
            setOnClickListener { launchEntry(entry) }
            setOnLongClickListener { showEntryMenu(entry); true }
        }
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(rowPad, rowPad, rowPad, rowPad)
        }

        val icon = ImageView(activity).apply {
            scaleType = ImageView.ScaleType.FIT_CENTER
            // Pre-set the layout size so rows without an icon don't
            // shift their text leftwards. ImageView default is
            // WRAP_CONTENT which collapses to 0 when the drawable is
            // null.
            adjustViewBounds = false
        }
        iconLoader.load(
            entry.iconPath,
            icon,
            if (entry.terminal) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback,
        )
        row.addView(
            icon,
            LinearLayout.LayoutParams(iconSizePx, iconSizePx).also {
                it.marginEnd = rowPad
            },
        )

        val column = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        column.addView(TextView(activity).apply {
            text = entry.name.ifEmpty { entry.id }
            textSize = 16f
        })
        if (entry.comment.isNotEmpty()) {
            column.addView(TextView(activity).apply {
                text = entry.comment
                textSize = 13f
                alpha = 0.7f
            })
        }
        row.addView(column, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))

        card.addView(row)
        return card
    }

    private fun launchTop() {
        val top = filteredEntries.firstOrNull() ?: return
        launchEntry(top)
    }

    /**
     * Fire-and-forget launch via [EntryLauncher]; failures surface from
     * there ([LaunchErrorActivity]). The query is cleared and the IME
     * dropped: the app's window (or the terminal) comes forward, and
     * this pane is what the user returns to.
     */
    private fun launchEntry(entry: LauncherEntry) {
        val now = SystemClock.uptimeMillis()
        if (now - lastLaunchMs < LAUNCH_DEBOUNCE_MS) return
        lastLaunchMs = now
        EntryLauncher.launch(activity.applicationContext, installation, entry)
        searchField.text.clear()
        searchField.clearFocus()
        val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.hideSoftInputFromWindow(searchField.windowToken, 0)
    }

    private companion object {
        /** Square icon edge in dp. ~48 is the standard list-row icon size
         *  per Material guidelines; bumped to 56 here because chroot apps
         *  rarely have icon sets that look crisp at the smaller size and
         *  the extra display surface helps with brand recognition. */
        const val ICON_SIZE_DP = 56f

        const val LAUNCH_DEBOUNCE_MS = 500L
    }
}
