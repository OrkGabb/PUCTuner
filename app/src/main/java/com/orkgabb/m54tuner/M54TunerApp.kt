package com.orkgabb.m54tuner

import android.app.Application
import com.orkgabb.m54tuner.root.RootShellManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * The su handshake is the slowest part of opening the app, and it is happening no matter what —
 * this app is useless without root. Starting it the instant the process exists, instead of waiting
 * for MainViewModel.bootstrap() to ask, lets it overlap with Activity/Compose startup rather than
 * run after it.
 */
class M54TunerApp : Application() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        scope.launch { RootShellManager.hasRoot() }
    }
}
