package me.phie.tawc.terminal

import com.termux.terminal.TerminalEmulator
import java.io.File
import java.io.IOException

/**
 * When an in-use shell may go back to pending (notes/terminal.md
 * "Pending vs in use"): its screen shows nothing but a prompt with an
 * empty input line, and nothing else runs in its session.
 */
internal object ShellIdle {

    /** Where the cursor sat when a pending shell got its first input,
     *  on a screen of [rows] x [columns]. */
    data class Anchor(val row: Int, val col: Int, val rows: Int, val columns: Int)

    fun anchorOf(emulator: TerminalEmulator): Anchor =
        Anchor(emulator.cursorRow, emulator.cursorCol, emulator.mRows, emulator.mColumns)

    /**
     * The screen reads as an untouched prompt: nothing at or after the
     * cursor, and either
     *
     * - the cursor is back at [promoted] — where it was when the pending
     *   shell got its first input, with no Enter since (typed, erased), or
     * - the screen was cleared (`clear`: no scrollback, a one-line prompt
     *   at the top) and the input line is empty: nothing typed since the
     *   last Enter ([lineDirty] false), or erased back to [lineStart].
     *
     * The screen alone can't tell a prompt from unsent input, hence the
     * caller's input tracking. Anything else — a resize, output above, a
     * multi-line prompt — doesn't count.
     */
    fun screenIsFresh(
        emulator: TerminalEmulator,
        promoted: Anchor?,
        lineDirty: Boolean,
        lineStart: Anchor?,
    ): Boolean {
        if (emulator.isAlternateBufferActive) return false
        val screen = emulator.screen
        val row = emulator.cursorRow
        val col = emulator.cursorCol
        val lastRow = emulator.mRows - 1
        val lastCol = emulator.mColumns - 1
        if (screen.getSelectedText(col, row, lastCol, lastRow).isNotBlank()) return false
        if (promoted.isAt(emulator)) return true
        if (lineDirty && !lineStart.isAt(emulator)) return false
        if (screen.activeTranscriptRows != 0 || col == 0) return false
        if (row > 0 && screen.getSelectedText(0, 0, lastCol, row - 1).isNotBlank()) return false
        return screen.getSelectedText(0, row, col - 1, row).isNotBlank()
    }

    private fun Anchor?.isAt(e: TerminalEmulator): Boolean =
        this != null && rows == e.mRows && columns == e.mColumns && row == e.cursorRow && col == e.cursorCol

    /**
     * Whether [shellPid] is the only process in its session (the shell
     * leads the pty's session). A foreground program, a background job
     * or a `nohup` child all share it; `setsid`'d ones don't, and
     * survive the shell anyway.
     */
    fun aloneInSession(shellPid: Int): Boolean {
        if (shellPid <= 0) return false
        val names = File("/proc").list() ?: return false
        for (name in names) {
            val pid = name.toIntOrNull() ?: continue
            if (pid == shellPid) continue
            val stat = try {
                File("/proc/$name/stat").readText()
            } catch (_: IOException) {
                continue // exited mid-scan
            }
            if (sessionOf(stat) == shellPid) return false
        }
        return true
    }

    /** Session id from a `/proc/<pid>/stat` line: the 4th field after
     *  the `(comm)`, which may itself hold spaces and parentheses. */
    fun sessionOf(stat: String): Int? {
        val close = stat.lastIndexOf(')')
        if (close < 0) return null
        val fields = stat.substring(close + 1).trim().split(' ')
        return fields.getOrNull(3)?.toIntOrNull()
    }
}
