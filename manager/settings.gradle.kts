@file:Suppress("UnstableApiUsage")

enableFeaturePreview("TYPESAFE_PROJECT_ACCESSORS")

pluginManagement {
    repositories {
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven("https://jitpack.io")
    }
}

rootProject.name = "KernelSU"
include(":app")

// Murasaki API SDK
include(":murasaki-api:api")
include(":murasaki-api:aidl")
include(":murasaki-api:provider")
include(":murasaki-api:shared")
