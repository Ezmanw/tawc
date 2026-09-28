package me.phie.tawc.terminal

import com.termux.terminal.TerminalEmulator
import com.termux.terminal.TerminalOutput
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** [ShellIdle] against a real (pure-Java) termux emulator. */
class ShellIdleTest {

    private object NullOutput : TerminalOutput() {
        override fun write(data: ByteArray, offset: Int, count: Int) {}
        override fun titleChanged(oldTitle: String?, newTitle: String?) {}
        override fun onCopyTextToClipboard(text: String?) {}
        override fun onPasteTextFromClipboard() {}
        override fun onBell() {}
        override fun onColorsChanged() {}
    }

    private fun emulator(vararg output: String): TerminalEmulator =
        TerminalEmulator(NullOutput, 40, 10, 8, 16, 100, null).also { e ->
            for (s in output) {
                val bytes = s.toByteArray()
                e.append(bytes, bytes.size)
            }
        }

    private fun TerminalEmulator.feed(s: String) {
        val bytes = s.toByteArray()
        append(bytes, bytes.size)
    }

    private fun fresh(
        e: TerminalEmulator,
        promoted: ShellIdle.Anchor? = null,
        lineDirty: Boolean = false,
        lineStart: ShellIdle.Anchor? = null,
    ) = ShellIdle.screenIsFresh(e, promoted, lineDirty, lineStart)

    private val clear = "\u001b[H\u001b[2J\u001b[3J"

    @Test
    fun clearedPromptIsFresh() {
        val e = emulator("some output\r\nmore\r\n~ # ls\r\nfoo bar\r\n", clear, "~ # ")
        assertTrue(fresh(e))
    }

    @Test
    fun clearedPromptWithUnsentInputIsNot() {
        // Only input tracking tells "~ # ls" (typed) from a prompt.
        val e = emulator(clear, "~ # ")
        val start = ShellIdle.anchorOf(e)
        e.feed("ls")
        assertFalse(fresh(e, lineDirty = true, lineStart = start))
        assertFalse(fresh(e, lineDirty = true))
        e.feed("\b \b\b \b")
        assertTrue(fresh(e, lineDirty = true, lineStart = start))
    }

    @Test
    fun blankClearedScreenWithoutPromptIsNot() {
        // `clear; read x`: nothing marks a prompt.
        assertFalse(fresh(emulator("x\r\n", clear)))
    }

    @Test
    fun scrollbackLeftIsNotCleared() {
        // Ctrl-L keeps scrollback, and a prompt below output isn't a clear.
        val lines = (1..20).joinToString("") { "line $it\r\n" }
        assertFalse(fresh(emulator(lines, "\u001b[H\u001b[2J", "~ # ")))
        assertFalse(fresh(emulator("out\r\n~ # ")))
    }

    @Test
    fun typedThenErasedReturnsToAnchor() {
        val e = emulator("motd\r\n~ # ")
        val anchor = ShellIdle.anchorOf(e)
        e.feed("ls -l")
        assertFalse(fresh(e, anchor, lineDirty = true, lineStart = anchor))
        e.feed("\b \b".repeat(5))
        assertTrue(fresh(e, anchor, lineDirty = true, lineStart = anchor))
        // Output left on screen: only the promotion anchor makes it fresh.
        assertFalse(fresh(e, null, lineDirty = true, lineStart = anchor))
    }

    @Test
    fun enterMovesPastTheAnchor() {
        val e = emulator("motd\r\n~ # ")
        val anchor = ShellIdle.anchorOf(e)
        e.feed("\r\n~ # ")
        assertFalse(fresh(e, anchor))
    }

    @Test
    fun altScreenIsNever() {
        val e = emulator(clear, "~ # ", "\u001b[?1049h")
        assertFalse(fresh(e, ShellIdle.anchorOf(e)))
    }

    @Test
    fun sessionFieldParsesPastParenthesesInComm() {
        assertEquals(4321, ShellIdle.sessionOf("1234 (a) b (c)) S 1 1234 4321 34816 1234 0"))
        assertEquals(77, ShellIdle.sessionOf("88 (bash) S 77 88 77 34816 88 4194304"))
        assertNull(ShellIdle.sessionOf("garbage"))
    }
}
