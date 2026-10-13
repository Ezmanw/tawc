package me.phie.tawc.launcher

import me.phie.tawc.GraphicsBackend
import me.phie.tawc.PointerEmulation

/**
 * The entry editor's form as plain values: loaded from the entry being
 * edited ([of]), read back from the widgets, and turned into store
 * fields on Save ([overrides] for a scanned entry, [shortcut] for a
 * shortcut). One model for the dirty check, the per-field reset
 * controls and the write, so they can't disagree. Pure; unit-tested by
 * `EntryFormTest`.
 */
internal data class EntryForm(
    val exec: String = "",
    val name: String = "",
    /** Icon name or in-rootfs path. Blank while an image is in effect,
     *  or (scanned entry) for the packaged value. */
    val icon: String = "",
    /** Store icon file in effect, or null. */
    val iconFile: String? = null,
    /** An imported image not stored yet; replaces [iconFile] on Save. */
    val pendingIcon: IconImport.Result? = null,
    val terminal: Boolean = false,
    /** Rows as typed, names trimmed; blank rows dropped. */
    val env: List<Pair<String, String>> = emptyList(),
    /** Override of the global setting, or null for none. */
    val graphics: GraphicsBackend? = null,
    val pointer: PointerEmulation? = null,
) {
    val hasImage: Boolean get() = iconFile != null || pendingIcon != null

    /**
     * A scanned [entry]'s overrides: each field that differs from its
     * packaged value, over [old] (which keeps `comment`, `hidden` and
     * fields this build doesn't know). A blank Command, Name or Icon
     * means the packaged value. [newIcon] is the file [pendingIcon] was
     * just stored as.
     */
    fun overrides(entry: LauncherEntry, old: EntryFields, newIcon: String? = null): EntryFields {
        val image = newIcon ?: iconFile
        return old.copy(
            name = textOverride(name, entry.packagedValue("name")),
            exec = textOverride(exec, entry.packagedValue("exec")),
            terminal = terminal.takeIf { it != entry.packagedTerminal },
            icon = if (image != null) null else textOverride(icon, entry.packagedValue("icon")),
            iconFile = image,
            env = envMap(),
            graphics = graphics?.key,
            pointer = pointer?.key,
        )
    }

    /** A shortcut's whole fields over [old]. A blank Name is left unset
     *  (shown as the command); Command is required of the caller. */
    fun shortcut(old: EntryFields, newIcon: String? = null): EntryFields {
        val image = newIcon ?: iconFile
        return old.copy(
            name = name.trim().ifEmpty { null },
            exec = exec.trim(),
            terminal = terminal,
            icon = if (image != null) null else icon.trim().ifEmpty { null },
            iconFile = image,
            env = envMap(),
            graphics = graphics?.key,
            pointer = pointer?.key,
        )
    }

    /** Env names that are not valid shell names or appear in more than
     *  one row; such a form must not be saved (a map can't hold both). */
    fun badEnvNames(): Set<String> {
        val counts = env.groupingBy { it.first }.eachCount()
        return counts.keys.filterTo(HashSet()) { !DesktopEntryFile.isValidEnvName(it) || counts.getValue(it) > 1 }
    }

    private fun envMap(): Map<String, String>? = env.toMap().takeIf { it.isNotEmpty() }

    companion object {
        /** The form showing a scanned [entry]'s effective values. */
        fun of(entry: LauncherEntry) = EntryForm(
            exec = entry.exec,
            name = entry.name,
            icon = if (entry.iconFile.isNotEmpty()) "" else entry.icon,
            iconFile = entry.iconFile.ifEmpty { null },
            terminal = entry.terminal,
            env = entry.env.toList(),
            graphics = GraphicsBackend.fromKeyOrNull(entry.graphics),
            pointer = PointerEmulation.fromKeyOrNull(entry.pointer),
        )

        /** The form for a shortcut's store [fields]. Terminal is
         *  [terminalDefault] when unset: a launch treats that as false,
         *  but a new shortcut starts checked (hand-made entries are
         *  usually CLI programs). */
        fun of(fields: EntryFields, terminalDefault: Boolean) = EntryForm(
            exec = fields.exec.orEmpty(),
            name = fields.name.orEmpty(),
            icon = if (fields.iconFile != null) "" else fields.icon.orEmpty(),
            iconFile = fields.iconFile,
            terminal = fields.terminal ?: terminalDefault,
            env = fields.env.orEmpty().toList(),
            graphics = GraphicsBackend.fromKeyOrNull(fields.graphics),
            pointer = PointerEmulation.fromKeyOrNull(fields.pointer),
        )

        /** [value] as an override of [packaged]: null when blank or the same. */
        private fun textOverride(value: String, packaged: String): String? =
            value.trim().takeIf { it.isNotEmpty() && it != packaged }
    }
}
