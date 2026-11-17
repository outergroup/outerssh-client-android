plugins {
    id("com.android.application")
}

// Bundles the server-side helpers from helpers/ (see README.md) as uncompressed assets, so
// OuterSSH can upload them straight from the APK. The ".bin" suffix stops the asset merge
// from unwrapping the gzip files. Also bundles OuterSSH's socket host display script.
val helperAssets = layout.buildDirectory.dir("generated/helper-assets").get().asFile
val prepareHelperAssets = tasks.register<Sync>("prepareHelperAssets") {
    from(rootProject.file("helpers")) {
        include("outerssh-helper-*.gz")
        rename { "$it.bin" }
    }
    from(rootProject.file("OuterSSH/OuterSSH/web")) {
        include("SocketHostDisplay.js")
    }
    into(helperAssets)
}
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("Assets")) dependsOn(prepareHelperAssets)
}

android {
    namespace = "com.example.outersshclient"
    compileSdk {
        version = release(37)
    }
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.example.outersshclient"
        minSdk = 33
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"
        externalNativeBuild {
            cmake {
                arguments += "-DOUTERSSH_ANDROID_CRYPTO=ON"
            }
        }
    }

    sourceSets["main"].assets.directories += helperAssets.path
    sourceSets["main"].java.directories += "../OuterSSH/OuterSSH/platform/android/java"
    androidResources {
        noCompress += "gz.bin"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

dependencies {
    implementation("androidx.webkit:webkit:1.17.1")
}
