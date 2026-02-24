import com.android.build.api.dsl.ApplicationDefaultConfig
import com.android.build.api.dsl.CommonExtension
import com.android.build.gradle.api.AndroidBasePlugin

plugins {
    alias(libs.plugins.agp.app) apply false
    alias(libs.plugins.agp.lib) apply false
    alias(libs.plugins.kotlin) apply false
    alias(libs.plugins.compose.compiler) apply false
    alias(libs.plugins.lsplugin.cmaker)
}

val buildAbiList = provider {
    val abi = findProperty("ABI")?.toString()
    if (abi != null) listOf(abi) else listOf("arm64-v8a", "x86_64", "armeabi-v7a")
}

cmaker {
    default {
        arguments.addAll(
            arrayOf(
                "-DANDROID_STL=none",
            )
        )
        abiFilters(*buildAbiList.get().toTypedArray())
    }
    buildTypes {
        if (it.name == "release") {
            arguments += "-DDEBUG_SYMBOLS_PATH=${layout.buildDirectory.asFile.get().absolutePath}/symbols"
        }
    }
}

val androidMinSdkVersion = 26
val androidTargetSdkVersion = 36
val androidCompileSdkVersion = 36
val androidBuildToolsVersion = "36.1.0"
val androidCompileNdkVersion by extra(libs.versions.ndk.get())
val androidCmakeVersion by extra("3.22.0+")
val androidSourceCompatibility = JavaVersion.VERSION_17
val androidTargetCompatibility = JavaVersion.VERSION_17
val managerVersionCode by extra(10000 - 3135 + getGitCommitCount())
val managerVersionName by extra(computeManagerVersionName())

fun getGitCommitCount(): Int {
    return providers.exec {
        commandLine("git", "rev-list", "--count", "HEAD")
    }.standardOutput.asText.get().trim().toInt()
}

/** Manager version from latest tag: v1.4.0 or v1.4.0-8char_hash (git describe --tags). */
fun computeManagerVersionName(): String {
    val describe = providers.exec {
        commandLine("git", "describe", "--tags", "--always", "--abbrev=8")
    }.standardOutput.asText.get().trim()
    // "v1.4.0" or "v1.4.0-1-g56b0efb0" -> "v1.4.0-56b0efb0"
    return if (describe.contains("-g")) {
        describe.replace(Regex("-\\d+-g"), "-")
    } else {
        describe
    }
}

subprojects {
    plugins.withType(AndroidBasePlugin::class.java) {
        extensions.configure(CommonExtension::class.java) {
            compileSdk = androidCompileSdkVersion
            ndkVersion = androidCompileNdkVersion
            buildToolsVersion = androidBuildToolsVersion

            defaultConfig {
                minSdk = androidMinSdkVersion
                if (this is ApplicationDefaultConfig) {
                    targetSdk = androidTargetSdkVersion
                    versionCode = managerVersionCode
                    versionName = managerVersionName
                }
                ndk {
                    abiFilters += (rootProject.findProperty("ABI")?.toString()?.let { listOf(it) } ?: listOf("arm64-v8a", "x86_64", "armeabi-v7a"))
                }
            }

            lint {
                abortOnError = true
                checkReleaseBuilds = false
            }

            compileOptions {
                sourceCompatibility = androidSourceCompatibility
                targetCompatibility = androidTargetCompatibility
            }
        }
    }

    tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile> {
        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }
}
