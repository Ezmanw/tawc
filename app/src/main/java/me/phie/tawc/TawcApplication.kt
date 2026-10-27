package me.phie.tawc

import android.app.Application
import android.util.Log
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.BootstrapCache
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.RootfsTmpSweeper
import me.phie.tawc.install.TawcInstaller
import me.phie.tawc.ops.OperationsNotificationCenter
import me.phie.tawc.session.SessionService
import kotlin.concurrent.thread

/**
 * Process-wide entry point. Used for cheap startup chores that have no
 * UI and shouldn't block onCreate of the launcher / compositor:
 *
 *  - Sweep stale bootstrap-tarball cache entries — the OS only evicts
 *    `cacheDir` under storage pressure, so a 200 MB tarball can squat
 *    on disk for months without our own TTL ([BootstrapCache.sweepStale]).
 *  - Start the dev exec broker (debug builds only — [DevHooks]).
 *
 * Per-install `nativeLibraryDir` (which moves between APKs as
 * `/data/app/~~<hash>/...`) is resolved fresh in
 * [InstallationMethod.startInside] each entry, so there's nothing
 * persistent to refresh here.
 */
class TawcApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        TawcRuntime.init(this)
    }
}

/**
 * The startup work [TawcApplication] does, callable from a host app's own Application when TAWC
 * is embedded as a library (Foldiux fork). Idempotent.
 */
object TawcRuntime {
    private const val TAG = "tawc-install"
    @Volatile private var started = false

    @Synchronized
    fun init(app: Application) {
        if (started) return
        started = true
        // Bind the SharedPreferences instance early so non-Activity
        // code (RootfsEnv on the broker thread, etc.) can read settings
        // without a Context handle. Cheap (memory-mapped pref file).
        Settings.init(app)
        // Start the per-op notification center before any service can
        // register an Operation. The center is the single owner of the
        // `tawc-operations` notification channel and the
        // OperationsRegistry → notification fan-out — see
        // me.phie.tawc.ops package KDoc.
        OperationsNotificationCenter.start(app)
        // Before anything can spawn into a rootfs.
        SessionService.install(app)
        thread(name = "tawc-startup", isDaemon = true) {
            // Production ando broker (run Android commands from rootfs
            // guests; notes/ando.md). Per-distro: one listener per
            // ando-enabled install. All build types; alive whenever the
            // app process is. On this thread because touching
            // NativeBridge triggers its `System.loadLibrary` of the
            // large compositor .so, which shouldn't block onCreate.
            try {
                CompositorService.ensureActivation(app)
            } catch (t: Throwable) {
                Log.w(TAG, "compositor activation start failed", t)
            }
            try {
                val appPaths = AppPaths.from(app)
                appPaths.shareDir.mkdirs()
                // Unlink the single shared ando socket older versions
                // bound here (before ando went per-distro).
                appPaths.legacyAndoSocket.delete()
                AndoBrokers.refresh(app)
            } catch (t: Throwable) {
                Log.w(TAG, "ando broker start failed", t)
            }
            try {
                val n = BootstrapCache(app).sweepStale()
                if (n > 0) Log.i(TAG, "Bootstrap cache: evicted $n stale entries")
            } catch (t: Throwable) {
                Log.w(TAG, "Bootstrap cache sweep failed", t)
            }
            // Refresh TAWC-installed files in every existing rootfs
            // when the app version stamp has changed since the last
            // install/refresh. No-op on cold app starts that follow a
            // run with the same `versionCode + lastUpdateTime` pair
            // (see CompositorService.currentExtractStamp). Per-rootfs
            // failures are logged and toasted inside [installAll].
            try {
                TawcInstaller.installAll(app, InstallationStore(app))
            } catch (t: Throwable) {
                Log.w(TAG, "TawcInstaller.installAll failed", t)
            }
            // Age-sweep every install's flash-backed /tmp (no init in
            // the rootfs means nothing else ever clears it). See
            // [RootfsTmpSweeper] for the design constraints.
            try {
                RootfsTmpSweeper.sweepAll(InstallationStore(app))
            } catch (t: Throwable) {
                Log.w(TAG, "rootfs /tmp sweep failed", t)
            }
        }
        // Dev-only exec broker and its action handlers. [DevHooks] has
        // two implementations at the same FQCN — the real one in
        // `src/debug/java`, an empty one in `src/release/java` — so the
        // broker isn't in the release APK at all. See notes/exec-broker.md.
        DevHooks.start(app)
    }
}
