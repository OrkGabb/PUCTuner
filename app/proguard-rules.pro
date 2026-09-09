
# libsu spawns its root shell through a service/reflection path; keep it whole.
-keep class com.topjohnwu.superuser.** { *; }
-dontwarn com.topjohnwu.superuser.**
