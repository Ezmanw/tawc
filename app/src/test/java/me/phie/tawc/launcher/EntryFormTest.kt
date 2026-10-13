package me.phie.tawc.launcher

import me.phie.tawc.GraphicsBackend
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** [EntryForm]: the editor's load → edit → store-fields logic. */
class EntryFormTest {

    private val firefox = LauncherEntry(
        id = "firefox", name = "Web", comment = "", exec = "firefox", terminal = false,
        iconPath = "", icon = "firefox", source = LauncherEntry.Source.DESKTOP,
        overridden = setOf("name", "env"), packaged = mapOf("name" to "Firefox"),
        env = mapOf("MOZ" to "1"),
    )

    @Test
    fun loadsEffectiveValuesOfAScannedEntry() {
        val f = EntryForm.of(firefox)
        assertEquals("Web", f.name)
        assertEquals("firefox", f.exec)
        assertEquals("firefox", f.icon)
        assertNull(f.iconFile)
        assertEquals(listOf("MOZ" to "1"), f.env)
        assertNull(f.graphics)
        // An image in effect blanks the icon field.
        val img = EntryForm.of(firefox.copy(iconFile = "web.png", overridden = setOf("iconFile")))
        assertEquals("", img.icon)
        assertEquals("web.png", img.iconFile)
    }

    @Test
    fun overridesHoldOnlyWhatDiffersFromPackaged() {
        val f = EntryForm.of(firefox)
        // Unchanged form: the name override stays, nothing else appears.
        assertEquals(EntryFields(name = "Web", env = mapOf("MOZ" to "1")), f.overrides(firefox, EntryFields()))
        // Typing the packaged value, or blank, drops the override;
        // whitespace doesn't count.
        assertNull(f.copy(name = " Firefox ").overrides(firefox, EntryFields()).name)
        assertNull(f.copy(name = "").overrides(firefox, EntryFields()).name)
        assertNull(f.copy(exec = "firefox").overrides(firefox, EntryFields()).exec)
        assertEquals("firefox -P x", f.copy(exec = "firefox -P x").overrides(firefox, EntryFields()).exec)
        assertEquals(true, f.copy(terminal = true).overrides(firefox, EntryFields()).terminal)
        assertNull(f.copy(terminal = false).overrides(firefox, EntryFields()).terminal)
        assertNull(f.copy(env = emptyList()).overrides(firefox, EntryFields()).env)
    }

    @Test
    fun overridesKeepFieldsTheFormDoesNotEdit() {
        val old = EntryFields(comment = "C", hidden = true, extra = mapOf("x" to "1"), graphics = "cpu")
        val o = EntryForm.of(firefox).overrides(firefox, old)
        assertEquals("C", o.comment)
        assertEquals(true, o.hidden)
        assertEquals(mapOf("x" to "1"), o.extra)
        // The form's (unset) graphics pick wins over the stored one.
        assertNull(o.graphics)
    }

    @Test
    fun imageReplacesIconTextAndViceVersa() {
        val f = EntryForm.of(firefox).copy(icon = "", iconFile = "web.png")
        var o = f.overrides(firefox, EntryFields())
        assertEquals("web.png", o.iconFile)
        assertNull(o.icon)
        // A just-stored import takes the new file name.
        o = f.copy(iconFile = null, pendingIcon = IconImport.Result(byteArrayOf(1), "new")).overrides(firefox, EntryFields(), newIcon = "new.png")
        assertEquals("new.png", o.iconFile)
        // A name instead: no file, and only a differing name is an override.
        o = f.copy(iconFile = null, icon = "firefox").overrides(firefox, EntryFields())
        assertNull(o.iconFile)
        assertNull(o.icon)
        assertEquals("other", f.copy(iconFile = null, icon = "other").overrides(firefox, EntryFields()).icon)
    }

    @Test
    fun shortcutStoresTheWholeForm() {
        val f = EntryForm(exec = " htop ", name = "", icon = " utilities-system-monitor ", terminal = true)
        assertEquals(
            EntryFields(exec = "htop", terminal = true, icon = "utilities-system-monitor"),
            f.shortcut(EntryFields()),
        )
        val g = f.copy(name = "Top", icon = "", iconFile = "top.png", env = listOf("A" to "1"), graphics = GraphicsBackend.LIBHYBRIS)
        assertEquals(
            EntryFields(
                name = "Top", exec = "htop", terminal = true, iconFile = "top.png",
                env = mapOf("A" to "1"), graphics = "libhybris", hidden = true,
            ),
            g.shortcut(EntryFields(hidden = true)),
        )
    }

    @Test
    fun loadsAShortcutWithTerminalDefault() {
        val stored = EntryFields(exec = "htop", iconFile = "top.png", icon = "ignored")
        val f = EntryForm.of(stored, terminalDefault = false)
        assertEquals("htop", f.exec)
        assertEquals("", f.icon)
        assertEquals("top.png", f.iconFile)
        assertEquals(false, f.terminal)
        assertEquals(true, EntryForm.of(EntryFields(), terminalDefault = true).terminal)
        assertEquals(false, EntryForm.of(EntryFields(terminal = false), terminalDefault = true).terminal)
    }

    @Test
    fun badEnvNamesAreInvalidOrRepeated() {
        val f = EntryForm(env = listOf("A" to "1", "1X" to "2", "B" to "3", "B" to "4", "" to "5"))
        assertEquals(setOf("1X", "B", ""), f.badEnvNames())
        assertEquals(emptySet<String>(), EntryForm(env = listOf("A" to "1", "B" to "2")).badEnvNames())
    }

    @Test
    fun packagedTerminalReadsTheScannedValue() {
        assertEquals(false, firefox.packagedTerminal)
        assertEquals(true, firefox.copy(terminal = true).packagedTerminal)
        assertEquals(true, firefox.copy(terminal = false, packaged = mapOf("terminal" to "true")).packagedTerminal)
    }
}
