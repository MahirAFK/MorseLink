package com.morselink.morselink_app

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    // The screen stays on while the app is open: during a test session the
    // phone must keep its Bluetooth link, GPS and internet relay going.
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine) // (registers the plugins)

        // The app's own folders, for lib/storage.dart (in place of the
        // path_provider plugin): "files" is private to the app (the
        // settings); "external" is Android/data/<app>/files, which a PC sees
        // over USB (the logs, when Download/ can't be written). Null if
        // there is none.
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "morselink/paths")
            .setMethodCallHandler { call, result ->
                if (call.method == "dirs") {
                    result.success(
                        mapOf(
                            "files" to filesDir.absolutePath,
                            "external" to getExternalFilesDir(null)?.absolutePath,
                        )
                    )
                } else {
                    result.notImplemented()
                }
            }

        // Keeping the app running with the screen off (lib/background.dart):
        // "show" starts the service or updates its notification's text and
        // answers whether it runs; "stop" ends it.
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "morselink/service")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "show" -> result.success(KeepAlive.show(this, call.argument<String>("text") ?: "MorseLink"))
                    "stop" -> {
                        KeepAlive.stop(this)
                        result.success(true)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    // The app closed (not just hidden): nothing left to keep running.
    override fun onDestroy() {
        if (isFinishing) {
            KeepAlive.stop(this)
        }
        super.onDestroy()
    }
}

// The notification of the service below, and starting / stopping it. Every
// step that Android may refuse is caught: the app then simply works as
// before v0.2.0 (paused with the screen off), and the next "show" tries
// again.
object KeepAlive {
    const val CHANNEL = "morselink_link"
    const val ID = 4207

    @Volatile var running = false // the service is in the foreground
    @Volatile var text = "MorseLink"

    // The notification: quiet (no sound, no pop-up), permanent, and a tap
    // opens the app.
    fun notification(ctx: Context): Notification {
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Board connection", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val open = PendingIntent.getActivity(
            ctx,
            0,
            Intent(ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        @Suppress("DEPRECATION")
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(ctx, CHANNEL) else Notification.Builder(ctx)
        return b.setContentTitle("MorseLink")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .setContentIntent(open)
            .build()
    }

    // Start the service, or (running already) just update the text. On
    // Android 13+ the notification shows only if notifications are allowed
    // for the app (Settings > Apps > MorseLink > Notifications); the service
    // works either way.
    fun show(ctx: Context, t: String): Boolean {
        text = t
        if (running) {
            try {
                if (Build.VERSION.SDK_INT < 33 ||
                    ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
                ) {
                    val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                    nm.notify(ID, notification(ctx))
                }
            } catch (e: Exception) {
            }
            return true
        }
        // Android 14+ runs a "connectedDevice" service only once Bluetooth
        // ("Nearby devices") is allowed, and would end the app otherwise:
        // start it after that (the app asks for it when it first connects).
        if (Build.VERSION.SDK_INT >= 31 &&
            ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }
        return try {
            val i = Intent(ctx, KeepAliveService::class.java)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
            true
        } catch (e: Exception) {
            false // (e.g. Android 12+ refuses while the app is in the background)
        }
    }

    fun stop(ctx: Context) {
        running = false
        try {
            ctx.stopService(Intent(ctx, KeepAliveService::class.java))
        } catch (e: Exception) {
        }
    }
}

// The foreground service itself: it does nothing but exist, which keeps
// Android from pausing the app (the Bluetooth link and the relay live in the
// app). Type "connectedDevice": it serves a Bluetooth device; Android 14+
// also wants the "Nearby devices" permission granted for it.
class KeepAliveService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        try {
            val n = KeepAlive.notification(this)
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(KeepAlive.ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            } else {
                startForeground(KeepAlive.ID, n)
            }
            KeepAlive.running = true
        } catch (e: Exception) {
            KeepAlive.running = false // (no permission yet: the app runs as before)
            stopSelf()
        }
        return START_NOT_STICKY
    }

    // The app was swiped away: the service goes with it.
    override fun onTaskRemoved(rootIntent: Intent?) {
        KeepAlive.running = false
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        KeepAlive.running = false
        super.onDestroy()
    }
}
