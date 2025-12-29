package com.example.code_wipe  // Make sure this matches your app package

import android.os.storage.StorageManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity: FlutterActivity() {
    private val CHANNEL = "com.yourdomain.deviceinfo"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            if (call.method == "getDiskInfo") {
                val storageManager = getSystemService(STORAGE_SERVICE) as StorageManager
                val volumes = storageManager.storageVolumes
                val diskInfoList = volumes.map { it.getDescription(this) }
                result.success(diskInfoList)
            } else {
                result.notImplemented()
            }
        }
    }
}
