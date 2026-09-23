plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.orkgabb.m54tuner"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.orkgabb.m54tuner"
        minSdk = 33
        targetSdk = 36
        versionCode = 16
        versionName = "0.13.1"
    }

    buildTypes {
        release {
            // R8 + resource shrinking. This is not cosmetic for a Compose app: a debug build runs
            // the UI without optimization and without profile-guided compilation, which on this
            // mid-range Exynos is the difference between a smooth list and a visibly dragging one.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Signed with the debug key so the release build can be side-loaded directly; this app
            // is not distributed through a store.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = "11"
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.activity:activity-compose:1.9.3")

    val composeBom = platform("androidx.compose:compose-bom:2025.01.00")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    // NOT material-icons-extended: that pulls in ~5000 icon classes (tens of MB in an
    // unminified debug dex) for icons this app doesn't use. Icons.Filled.Info/Refresh/Warning —
    // the only three used — already ship in material3's default icon set at no extra cost.

    implementation("androidx.datastore:datastore-preferences:1.1.1")

    val libsuVersion = "6.0.0"
    implementation("com.github.topjohnwu.libsu:core:$libsuVersion")

    testImplementation("junit:junit:4.13.2")

    debugImplementation("androidx.compose.ui:ui-tooling")
}
