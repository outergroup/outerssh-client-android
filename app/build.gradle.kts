import java.net.URI
import java.security.MessageDigest

plugins {
    id("com.android.application")
}

// Downloads the OuterSSH server helpers listed in helpers.sha256 into helpers/ and checks
// each against its hash. Files that are already there with the right hash aren't
// downloaded again.
val outersshHelperVersion = "0.5.4"
val downloadHelpers = tasks.register("downloadHelpers") {
    val pins = rootProject.file("helpers.sha256")
    val directory = rootProject.file("helpers")
    val version = outersshHelperVersion
    inputs.file(pins)
    outputs.dir(directory)
    doLast {
        fun sha256(file: File) = MessageDigest.getInstance("SHA-256")
            .digest(file.readBytes()).joinToString("") { "%02x".format(it) }
        directory.mkdirs()
        for (line in pins.readLines().filter { Regex("^[0-9a-f]{64}  ").containsMatchIn(it) }) {
            val (hash, name) = line.split("  ", limit = 2)
            val file = File(directory, name)
            if (file.isFile && sha256(file) == hash) continue
            val partial = File(directory, "$name.partial")
            URI("https://outerssh.org/download/outerssh/$version/$name").toURL().openStream().use { input ->
                partial.outputStream().use { input.copyTo(it) }
            }
            check(sha256(partial) == hash) { "$name doesn't match its hash in helpers.sha256" }
            check(partial.renameTo(file)) { "Couldn't move $name into helpers/" }
        }
    }
}

// Bundles the server-side helpers from helpers/ (see README.md) as uncompressed assets, so
// OuterSSH can upload them straight from the APK. The ".bin" suffix stops the asset merge
// from unwrapping the gzip files. Also bundles OuterSSH's socket host display script.
val helperAssets = layout.buildDirectory.dir("generated/helper-assets").get().asFile
val prepareHelperAssets = tasks.register<Sync>("prepareHelperAssets") {
    dependsOn(downloadHelpers)
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
