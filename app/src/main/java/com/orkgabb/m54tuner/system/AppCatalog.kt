package com.orkgabb.m54tuner.system

import android.content.Context
import android.content.Intent
import androidx.core.graphics.drawable.toBitmap
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

data class AppInfo(
    val pkg: String,
    val label: String,
    val icon: ImageBitmap?,
    val isGame: Boolean,
)

/**
 * Launchable installed apps, with their real names and icons. The old picker listed raw package
 * names from `pm list packages -3`, which made choosing which app to AOT-compile or force-stop a
 * guessing game — this reads the same set through the PackageManager instead, no root involved.
 */
object AppCatalog {

    @Volatile
    private var cache: List<AppInfo>? = null

    suspend fun load(context: Context, refresh: Boolean = false): List<AppInfo> {
        cache?.takeIf { !refresh }?.let { return it }
        return withContext(Dispatchers.IO) {
            val pm = context.packageManager
            val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            val resolved = pm.queryIntentActivities(intent, 0)
            val seen = HashSet<String>()
            val list = ArrayList<AppInfo>()
            for (ri in resolved) {
                val ai = ri.activityInfo?.applicationInfo ?: continue
                if (!seen.add(ai.packageName)) continue
                if (ai.packageName == context.packageName) continue
                val icon = runCatching {
                    pm.getApplicationIcon(ai).toBitmap(96, 96).asImageBitmap()
                }.getOrNull()
                list.add(
                    AppInfo(
                        pkg = ai.packageName,
                        label = runCatching { pm.getApplicationLabel(ai).toString() }
                            .getOrDefault(ai.packageName),
                        icon = icon,
                        // Category is what Play declares; it is what the Game Booster keys off too,
                        // so it is a good default sort for "which of these is a game".
                        isGame = ai.category == android.content.pm.ApplicationInfo.CATEGORY_GAME,
                    )
                )
            }
            list.sortWith(compareByDescending<AppInfo> { it.isGame }.thenBy { it.label.lowercase() })
            list.also { cache = it }
        }
    }
}
