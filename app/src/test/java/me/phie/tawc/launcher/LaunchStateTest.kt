package me.phie.tawc.launcher

import org.junit.Assert.assertEquals
import org.junit.Test

class LaunchStateTest {

    private fun run(vararg events: LaunchEvent): LaunchState =
        events.fold(LaunchState.Waiting as LaunchState, ::reduce)

    @Test
    fun windowThenFirstFrame() = assertEquals(LaunchState.Shown, run(LaunchEvent.Matched, LaunchEvent.Shown))

    @Test
    fun nonZeroExitShowsLogAtOnce() =
        assertEquals(LaunchState.Exited(3), run(LaunchEvent.Exited(3, sessionAlive = false)))

    @Test
    fun cleanExitWaitsForHandoffThenQuits() {
        assertEquals(LaunchState.Handoff, run(LaunchEvent.Exited(0, sessionAlive = false)))
        assertEquals(LaunchState.Quit, run(LaunchEvent.Exited(0, sessionAlive = false), LaunchEvent.GraceOver))
        assertEquals(LaunchState.Matched, run(LaunchEvent.Exited(0, sessionAlive = false), LaunchEvent.Matched, LaunchEvent.GraceOver))
    }

    /** A launcher script that backgrounds the program and exits 0. */
    @Test
    fun cleanExitWithLiveSessionKeepsWaiting() =
        assertEquals(LaunchState.Waiting, run(LaunchEvent.Exited(0, sessionAlive = true)))

    @Test
    fun timeoutShowsLogAndALateWindowStillMatches() {
        assertEquals(LaunchState.TimedOut, run(LaunchEvent.TimeoutOver))
        assertEquals(LaunchState.Shown, run(LaunchEvent.TimeoutOver, LaunchEvent.Matched, LaunchEvent.Shown))
        assertEquals(LaunchState.Exited(0), run(LaunchEvent.TimeoutOver, LaunchEvent.Exited(0, sessionAlive = false)))
    }

    @Test
    fun timeoutIgnoredOnceMatched() = assertEquals(LaunchState.Matched, run(LaunchEvent.Matched, LaunchEvent.TimeoutOver))

    @Test
    fun exitAfterWindowIsIgnored() =
        assertEquals(LaunchState.Shown, run(LaunchEvent.Matched, LaunchEvent.Shown, LaunchEvent.Exited(1, sessionAlive = false)))

    @Test
    fun earlyWindowCloseWaitsForExitCode() {
        val shown = arrayOf(LaunchEvent.Matched, LaunchEvent.Shown)
        val closed = LaunchEvent.WindowClosed(early = true, exitCode = null)
        assertEquals(LaunchState.Closing, run(*shown, closed))
        assertEquals(LaunchState.Exited(230), run(*shown, closed, LaunchEvent.Exited(230, sessionAlive = false)))
        assertEquals(LaunchState.Closed, run(*shown, closed, LaunchEvent.Exited(0, sessionAlive = false)))
        assertEquals(LaunchState.Closed, run(*shown, closed, LaunchEvent.ClosingOver))
    }

    @Test
    fun earlyWindowCloseAfterExit() {
        assertEquals(LaunchState.Exited(230), run(LaunchEvent.Matched, LaunchEvent.WindowClosed(true, 230)))
        assertEquals(LaunchState.Closed, run(LaunchEvent.Matched, LaunchEvent.Shown, LaunchEvent.WindowClosed(true, 0)))
    }

    @Test
    fun lateWindowCloseIsIgnored() =
        assertEquals(LaunchState.Shown, run(LaunchEvent.Matched, LaunchEvent.Shown, LaunchEvent.WindowClosed(false, 1)))

    @Test
    fun spawnFailureShowsLog() =
        assertEquals(LaunchState.Failed("boom"), run(LaunchEvent.Failed("boom"), LaunchEvent.TimeoutOver))

    @Test
    fun detachOnlyFromWaiting() {
        assertEquals(LaunchState.Detached, run(LaunchEvent.Detached))
        assertEquals(LaunchState.Matched, run(LaunchEvent.Matched, LaunchEvent.Detached))
    }
}
