package me.phie.tawc.embed

import android.content.Context
import android.os.Handler
import android.os.Looper
import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.launcher.EntryLauncher
import me.phie.tawc.launcher.Launch
import me.phie.tawc.launcher.LauncherEntry
import me.phie.tawc.session.SessionExit

/**
 * The small public surface a host app uses when TAWC is embedded as a library (Foldiux fork).
 * Everything here delegates to TAWC's own code paths.
 */
object TawcEmbed {
    /**
     * Launches [exec] inside [installId]'s rootfs as a GUI program, behind TAWC's launch splash.
     * Returns the launch, whose state tracks the program's window, or null if the installation
     * or its method is unavailable.
     */
    fun launchGui(context: Context, installId: String, name: String, exec: String): Launch? {
        val app = context.applicationContext
        val inst = InstallationStore(app).load(installId) ?: return null
        val method = InstallationMethod.forKey(app, inst.method) ?: return null
        val rootfs = InstallationStore(app).rootfsDir(inst.id).absolutePath
        val entry = LauncherEntry(id = "embed:${name.lowercase()}", name = name, comment = "", exec = exec, terminal = false, iconPath = "")
        return EntryLauncher.launchGui(app, inst.id, method, rootfs, entry, graphics = null)
    }

    /** Kills every program TAWC started, like the session notification's Exit action. */
    fun exitAll(context: Context) {
        val app = context.applicationContext
        Handler(Looper.getMainLooper()).post { SessionExit.killEverything(app) }
    }
}
