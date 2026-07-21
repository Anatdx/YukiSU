package com.anatdx.yukisu.update

import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.util.Log
import androidx.core.content.FileProvider
import androidx.core.content.pm.PackageInfoCompat
import com.anatdx.yukisu.BuildConfig
import com.anatdx.yukisu.ksuApp
import com.anatdx.yukisu.ui.util.KsuCli
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import okhttp3.Call
import okhttp3.CacheControl
import okhttp3.Callback
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.Request
import okhttp3.Response
import org.bouncycastle.openpgp.PGPCompressedData
import org.bouncycastle.openpgp.PGPObjectFactory
import org.bouncycastle.openpgp.PGPPublicKey
import org.bouncycastle.openpgp.PGPPublicKeyRingCollection
import org.bouncycastle.openpgp.PGPSignature
import org.bouncycastle.openpgp.PGPSignatureList
import org.bouncycastle.openpgp.PGPUtil
import org.bouncycastle.openpgp.operator.bc.BcKeyFingerprintCalculator
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest
import java.util.Date
import java.util.concurrent.TimeUnit
import java.util.zip.ZipInputStream
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

data class CiRun(
    val runId: Long,
    val versionCode: Int,
    val commitSha: String,
    val commitMessage: String,
)

data class PreparedCiUpdate(
    val apk: File,
    val signature: File,
)

sealed interface CiInstallResult {
    data object RootInstalled : CiInstallResult
    data object SystemInstallerStarted : CiInstallResult
}

object CiUpdateManager {
    private const val TAG = "CiUpdateManager"
    private const val RUNS_API =
        "https://api.github.com/repos/Anatdx/YukiSU/actions/workflows/build-manager.yml/runs" +
            "?branch=main&status=success&per_page=1&exclude_pull_requests=true"
    private const val PAGES_METADATA_URL = "https://ci.yukisu.anatdx.com/ci-update.json"
    private const val PAGES_METADATA_SIGNATURE_URL = "https://ci.yukisu.anatdx.com/ci-update.sig"
    private const val NIGHTLY_METADATA_URL =
        "https://nightly.link/Anatdx/YukiSU/workflows/build-manager/main/" +
            "Manager-update-metadata.zip"
    private const val PAGES_ARCHIVE_URL =
        "https://ci.yukisu.anatdx.com/Manager-arm64-v8a.zip"
    private const val NIGHTLY_ARCHIVE_URL =
        "https://nightly.link/Anatdx/YukiSU/workflows/build-manager/main/Manager-arm64-v8a.zip"
    private const val APK_NAME = "app-release.apk"
    private const val SIGNATURE_NAME = "app-release.sig"
    private const val METADATA_NAME = "ci-update.json"
    private const val METADATA_SIGNATURE_NAME = "ci-update.sig"
    private const val CI_RUN_ID_META_DATA = "com.anatdx.yukisu.CI_RUN_ID"
    private const val PRIMARY_KEY_FINGERPRINT = "71B2B58C2A543472BE0DA0D8F580A2CEEF67DC98"
    // Extend this set only when a new CI signing subkey is intentionally approved.
    private val ALLOWED_SIGNING_SUBKEY_FINGERPRINTS = setOf(
        "C09CE484EEA3F2D88E9CDCC8EBDB0D663D7AB2F6",
    )

    private const val MAX_ARCHIVE_BYTES = 300L * 1024 * 1024
    private const val MAX_APK_BYTES = 250L * 1024 * 1024
    private const val MAX_SIGNATURE_BYTES = 256L * 1024
    private const val MAX_METADATA_ARCHIVE_BYTES = 1024L * 1024
    private const val MAX_METADATA_BYTES = 256L * 1024
    private const val METADATA_SOURCE_TIMEOUT_MS = 5_000L
    private const val METADATA_CHECK_DEDUPLICATION_MS = 5_000L
    private const val CI_MANAGER_VERSION_CODE_BASE = 10_000 - 3_135

    private val metadataCheckMutex = Mutex()
    private var lastMetadataCheckAt = 0L
    private var lastMetadataCheck: Result<CiRun?>? = null

    private val metadataClient by lazy {
        ksuApp.okhttpClient.newBuilder()
            .connectTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .readTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .writeTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .callTimeout(METADATA_SOURCE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
            .build()
    }

    suspend fun latestSuccessfulMainRun(force: Boolean = false): CiRun? = metadataCheckMutex.withLock {
        val now = SystemClock.elapsedRealtime()
        val previous = lastMetadataCheck
        if (!force && previous != null && now - lastMetadataCheckAt < METADATA_CHECK_DEDUPLICATION_MS) {
            return previous.getOrThrow()
        }

        val result = try {
            Result.success(loadLatestSuccessfulMainRun())
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            Result.failure(error)
        }
        lastMetadataCheck = result
        lastMetadataCheckAt = SystemClock.elapsedRealtime()
        result.getOrThrow()
    }

    private suspend fun loadLatestSuccessfulMainRun(): CiRun? = withContext(Dispatchers.IO) {
        val sources: List<Pair<String, suspend () -> CiRun?>> = listOf(
            "Cloudflare Pages" to { latestCloudflareSignedMainRun() },
            "nightly.link" to { latestNightlySignedMainRun() },
            "GitHub API" to { latestSuccessfulMainRunFromApi() },
        )
        val failures = mutableListOf<Throwable>()
        for ((name, source) in sources) {
            try {
                return@withContext withTimeout(METADATA_SOURCE_TIMEOUT_MS) { source() }
            } catch (timeout: TimeoutCancellationException) {
                val error = IOException("$name CI metadata source timed out", timeout)
                Log.w(TAG, error.message, error)
                failures += error
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.w(TAG, "$name CI metadata source failed", error)
                failures += error
            }
        }

        throw IOException("All CI metadata sources failed").apply {
            failures.forEach(::addSuppressed)
        }
    }

    private suspend fun latestCloudflareSignedMainRun(): CiRun = coroutineScope {
        val updateDir = metadataCacheDir("ci-update-metadata-pages")
        val metadata = File(updateDir, METADATA_NAME)
        val signature = File(updateDir, METADATA_SIGNATURE_NAME)
        metadata.delete()
        signature.delete()

        try {
            awaitAll(
                async {
                    downloadMetadataFile(
                        url = PAGES_METADATA_URL,
                        target = metadata,
                        maxBytes = MAX_METADATA_BYTES,
                    )
                },
                async {
                    downloadMetadataFile(
                        url = PAGES_METADATA_SIGNATURE_URL,
                        target = signature,
                        maxBytes = MAX_SIGNATURE_BYTES,
                    )
                },
            )
            verifyAndParseSignedMetadata(metadata, signature)
        } finally {
            metadata.delete()
            signature.delete()
        }
    }

    private suspend fun latestNightlySignedMainRun(): CiRun {
        val updateDir = metadataCacheDir("ci-update-metadata-nightly")
        val archive = File(updateDir, "Manager-update-metadata.zip")
        val metadata = File(updateDir, METADATA_NAME)
        val signature = File(updateDir, METADATA_SIGNATURE_NAME)
        archive.delete()
        metadata.delete()
        signature.delete()

        try {
            val request = Request.Builder()
                .url(NIGHTLY_METADATA_URL)
                .cacheControl(CacheControl.FORCE_NETWORK)
                .build()
            archive.writeBytes(requestMetadata(request, MAX_METADATA_ARCHIVE_BYTES).body)
            extractExpectedFiles(
                archive = archive,
                expected = mapOf(
                    METADATA_NAME to (metadata to MAX_METADATA_BYTES),
                    METADATA_SIGNATURE_NAME to (signature to MAX_SIGNATURE_BYTES),
                ),
            )
            return verifyAndParseSignedMetadata(metadata, signature)
        } finally {
            archive.delete()
            metadata.delete()
            signature.delete()
        }
    }

    private suspend fun latestSuccessfulMainRunFromApi(): CiRun? {
        val request = Request.Builder()
            .url(RUNS_API)
            .cacheControl(CacheControl.FORCE_NETWORK)
            .header("Accept", "application/vnd.github+json")
            .header("X-GitHub-Api-Version", "2022-11-28")
            .build()

        val response = requestMetadata(request, MAX_METADATA_ARCHIVE_BYTES)
        val run = parseLatestSuccessfulMainRun(response.body.decodeToString()) ?: return null
        return CiRun(
            runId = run.runId,
            versionCode = requestManagerVersionCode(run.commitSha),
            commitSha = run.commitSha,
            commitMessage = run.commitMessage,
        )
    }

    private fun metadataCacheDir(name: String): File = File(ksuApp.cacheDir, name).apply {
        check(isDirectory || mkdirs()) { "Cannot create the CI metadata cache" }
    }

    private suspend fun downloadMetadataFile(
        url: String,
        target: File,
        maxBytes: Long,
    ) {
        val request = Request.Builder()
            .url(url)
            .cacheControl(CacheControl.FORCE_NETWORK)
            .build()
        target.writeBytes(requestMetadata(request, maxBytes).body)
    }

    private suspend fun requestMetadata(
        request: Request,
        maxBytes: Long,
    ): MetadataHttpResponse = suspendCancellableCoroutine { continuation ->
        val call = metadataClient.newCall(request)
        continuation.invokeOnCancellation { call.cancel() }
        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                if (continuation.isActive) continuation.resumeWithException(e)
            }

            override fun onResponse(call: Call, response: Response) {
                response.use {
                    try {
                        check(response.isSuccessful) {
                            "${request.url.host} returned HTTP ${response.code}"
                        }
                        val body = response.body
                            ?: error("${request.url.host} returned an empty response")
                        val contentLength = body.contentLength()
                        check(contentLength < 0 || contentLength <= maxBytes) {
                            "${request.url.host} response is too large"
                        }
                        val output = ByteArrayOutputStream()
                        body.byteStream().use { input ->
                            copyWithLimit(input, output, maxBytes)
                        }
                        if (continuation.isActive) {
                            continuation.resume(
                                MetadataHttpResponse(
                                    body = output.toByteArray(),
                                    linkHeader = response.header("Link"),
                                )
                            )
                        }
                    } catch (error: Exception) {
                        if (continuation.isActive) continuation.resumeWithException(error)
                    }
                }
            }
        })
    }

    private fun verifyAndParseSignedMetadata(metadata: File, signature: File): CiRun {
        verifyDetachedSignature(ksuApp, metadata, signature)
        return parseSignedCiMetadata(metadata.readText())
    }

    private suspend fun requestManagerVersionCode(commitSha: String): Int {
        val request = Request.Builder()
            .url("https://api.github.com/repos/Anatdx/YukiSU/commits?sha=$commitSha&per_page=1")
            .cacheControl(CacheControl.FORCE_NETWORK)
            .header("Accept", "application/vnd.github+json")
            .header("X-GitHub-Api-Version", "2022-11-28")
            .build()
        val response = requestMetadata(request, MAX_METADATA_BYTES)
        check(JSONArray(response.body.decodeToString()).length() > 0) {
            "GitHub returned an empty commit history"
        }
        val commitCount = response.linkHeader
            ?.split(',')
            ?.asSequence()
            ?.map(String::trim)
            ?.firstOrNull { it.endsWith("rel=\"last\"") }
            ?.substringAfter('<')
            ?.substringBefore('>')
            ?.toHttpUrlOrNull()
            ?.queryParameter("page")
            ?.toIntOrNull()
            ?: 1
        return CI_MANAGER_VERSION_CODE_BASE + commitCount
    }

    private fun parseLatestSuccessfulMainRun(body: String): GitHubCiRun? {
        val runs = JSONObject(body).getJSONArray("workflow_runs")
        if (runs.length() == 0) return null

        val run = runs.getJSONObject(0)
        check(run.optString("head_branch") == "main") { "GitHub returned a non-main CI run" }
        check(run.optString("event") in setOf("push", "workflow_dispatch")) {
            "GitHub returned an unsupported CI event"
        }
        check(run.optString("conclusion") == "success") { "GitHub returned an unsuccessful CI run" }
        val commitSha = run.optString("head_sha")
        val commitMessage = run.optJSONObject("head_commit")
            ?.optString("message")
            ?.takeIf { it.isNotBlank() }
            ?: run.optString("display_title")
        return GitHubCiRun(
            runId = run.getLong("id"),
            commitSha = commitSha,
            commitMessage = commitMessage,
        )
    }

    private fun parseSignedCiMetadata(body: String): CiRun {
        val metadata = JSONObject(body)
        check(metadata.optInt("schema_version") == 1) { "Unsupported CI metadata schema" }
        check(metadata.optString("repository") == "Anatdx/YukiSU") {
            "CI metadata repository does not match"
        }
        check(metadata.optString("workflow") == "build-manager.yml") {
            "CI metadata workflow does not match"
        }
        check(metadata.optString("branch") == "main") { "CI metadata branch does not match" }
        val runId = metadata.optLong("run_id", -1L)
        check(runId > 0L) { "CI metadata run ID is invalid" }
        val versionCode = metadata.optInt("version_code", -1)
        check(versionCode > 0) { "CI metadata version code is invalid" }
        val commitSha = metadata.optString("commit_sha")
        check(commitSha.matches(Regex("[0-9a-fA-F]{40}"))) { "CI metadata commit SHA is invalid" }
        return CiRun(
            runId = runId,
            versionCode = versionCode,
            commitSha = commitSha,
            commitMessage = metadata.optString("commit_message"),
        )
    }

    private data class MetadataHttpResponse(
        val body: ByteArray,
        val linkHeader: String?,
    )

    private data class GitHubCiRun(
        val runId: Long,
        val commitSha: String,
        val commitMessage: String,
    )

    suspend fun downloadAndExtract(
        context: Context,
        onProgress: (Int) -> Unit,
    ): PreparedCiUpdate = withContext(Dispatchers.IO) {
        val updateDir = File(context.cacheDir, "ci-update").apply {
            check(isDirectory || mkdirs()) { "Cannot create the CI update cache" }
        }
        val archive = File(updateDir, "Manager-arm64-v8a.zip")
        val apk = File(updateDir, APK_NAME)
        val signature = File(updateDir, SIGNATURE_NAME)
        val sources = listOf(
            "Cloudflare Pages" to PAGES_ARCHIVE_URL,
            "nightly.link" to NIGHTLY_ARCHIVE_URL,
        )
        val failures = mutableListOf<Throwable>()

        for ((name, url) in sources) {
            archive.delete()
            apk.delete()
            signature.delete()
            withContext(Dispatchers.Main) { onProgress(0) }
            try {
                downloadUpdateArchive(name, url, archive, onProgress)
                extractExpectedFiles(
                    archive = archive,
                    expected = mapOf(
                        APK_NAME to (apk to MAX_APK_BYTES),
                        SIGNATURE_NAME to (signature to MAX_SIGNATURE_BYTES),
                    ),
                )
                return@withContext PreparedCiUpdate(apk, signature)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                val failure = IOException("$name CI artifact source failed", error)
                Log.w(TAG, failure.message, failure)
                failures += failure
            } finally {
                archive.delete()
            }
        }

        apk.delete()
        signature.delete()
        throw IOException("All CI artifact sources failed").apply {
            failures.forEach(::addSuppressed)
        }
    }

    private suspend fun downloadUpdateArchive(
        sourceName: String,
        url: String,
        archive: File,
        onProgress: (Int) -> Unit,
    ) {
        val request = Request.Builder().url(url).cacheControl(CacheControl.FORCE_NETWORK).build()
        ksuApp.okhttpClient.newCall(request).execute().use { response ->
            check(response.isSuccessful) { "$sourceName returned HTTP ${response.code}" }
            val body = response.body ?: error("$sourceName returned an empty response")
            val contentLength = body.contentLength()
            check(contentLength < 0 || contentLength <= MAX_ARCHIVE_BYTES) {
                "$sourceName CI artifact is too large"
            }
            body.byteStream().use { input ->
                archive.outputStream().buffered().use { output ->
                    copyDownloadWithProgress(
                        input = input,
                        output = output,
                        contentLength = contentLength,
                        onProgress = onProgress,
                    )
                }
            }
        }
    }

    fun verify(context: Context, run: CiRun, update: PreparedCiUpdate) {
        verifyDetachedSignature(context, update.apk, update.signature)
        verifyApk(context, run, update.apk)
    }

    suspend fun install(context: Context, apk: File): CiInstallResult = withContext(Dispatchers.IO) {
        if (KsuCli.SHELL.isRoot) {
            val temporaryApk = File("/data/local/tmp", "yukisu-ci-update-${Process.myPid()}.apk")
            val source = shellQuote(apk.absolutePath)
            val target = shellQuote(temporaryApk.absolutePath)
            val command =
                "cp $source $target && chmod 0644 $target && pm install -r $target; " +
                    "result=\$?; rm -f $target; exit \$result"
            val result = KsuCli.SHELL.newJob().add(command).exec()
            check(result.isSuccess) {
                (result.err + result.out).joinToString("\n").ifBlank { "Root package install failed" }
            }
            apk.delete()
            updateSignatureSibling(apk).delete()
            CiInstallResult.RootInstalled
        } else {
            withContext(Dispatchers.Main) {
                val uri = FileProvider.getUriForFile(
                    context,
                    "${context.packageName}.fileprovider",
                    apk,
                )
                val intent = Intent(Intent.ACTION_VIEW)
                    .setDataAndType(uri, "application/vnd.android.package-archive")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
            }
            CiInstallResult.SystemInstallerStarted
        }
    }

    private fun extractExpectedFiles(
        archive: File,
        expected: Map<String, Pair<File, Long>>,
    ) {
        val extracted = mutableSetOf<String>()
        ZipInputStream(BufferedInputStream(FileInputStream(archive))).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) {
                    zip.closeEntry()
                    continue
                }
                val normalizedName = entry.name.removePrefix("./")
                check(entry.name == normalizedName || entry.name == "./$normalizedName") {
                    "Unsafe path in CI artifact"
                }
                val target = expected[normalizedName]
                    ?: error("Unexpected file in CI artifact: ${entry.name}")
                check('/' !in normalizedName && '\\' !in normalizedName) { "Unsafe path in CI artifact" }
                check(extracted.add(normalizedName)) { "Duplicate file in CI artifact: $normalizedName" }
                check(entry.size < 0 || entry.size <= target.second) { "$normalizedName is too large" }
                target.first.outputStream().buffered().use { output ->
                    copyWithLimit(zip, output, target.second)
                }
                check(target.first.length() > 0) { "$normalizedName is empty" }
                zip.closeEntry()
            }
        }
        check(extracted == expected.keys) { "CI artifact does not contain the expected APK/signature pair" }
    }

    private fun verifyDetachedSignature(context: Context, apk: File, signatureFile: File) {
        val keyRings = context.assets.open("ci-update-public-key.asc").use { keyInput ->
            PGPPublicKeyRingCollection(
                PGPUtil.getDecoderStream(keyInput),
                BcKeyFingerprintCalculator(),
            )
        }
        val signature = readDetachedSignature(signatureFile)
        val signingKey = keyRings.getPublicKey(signature.keyID)
            ?: error("PGP signature was made by an unknown key")

        val owningRing = keyRings.keyRings.asSequence().firstOrNull { ring ->
            ring.getPublicKey(signingKey.keyID) != null
        } ?: error("PGP signing key is not in the embedded keyring")
        check(fingerprint(owningRing.publicKey) == PRIMARY_KEY_FINGERPRINT) {
            "PGP signing key does not belong to the pinned primary key"
        }
        check(!signingKey.isMasterKey) { "The CI artifact must be signed by an allowed subkey" }
        check(fingerprint(signingKey) in ALLOWED_SIGNING_SUBKEY_FINGERPRINTS) {
            "PGP signing subkey is not allowed"
        }
        check(!signingKey.hasRevocation()) { "PGP signing subkey is revoked" }
        check(signature.signatureType == PGPSignature.BINARY_DOCUMENT) {
            "PGP signature has an unexpected type"
        }
        checkSignatureTime(signingKey, signature.creationTime)

        signature.init(
            Ed25519PgpContentVerifierProvider,
            signingKey,
        )
        apk.inputStream().buffered().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                signature.update(buffer, 0, count)
            }
        }
        check(signature.verify()) { "PGP signature verification failed" }
    }

    private fun readDetachedSignature(file: File): PGPSignature {
        PGPUtil.getDecoderStream(file.inputStream().buffered()).use { decoded ->
            return findSignature(PGPObjectFactory(decoded, BcKeyFingerprintCalculator()))
                ?: error("Detached PGP signature is missing")
        }
    }

    private fun findSignature(factory: PGPObjectFactory): PGPSignature? {
        while (true) {
            when (val item = factory.nextObject() ?: return null) {
                is PGPSignatureList -> {
                    check(item.size() == 1) { "Expected exactly one detached PGP signature" }
                    return item[0]
                }
                is PGPCompressedData -> {
                    val nested = PGPObjectFactory(item.dataStream, BcKeyFingerprintCalculator())
                    findSignature(nested)?.let { return it }
                }
            }
        }
    }

    private fun checkSignatureTime(key: PGPPublicKey, signatureTime: Date) {
        check(!signatureTime.before(key.creationTime)) { "PGP signature predates its signing key" }
        val validSeconds = key.validSeconds
        if (validSeconds > 0) {
            val expiresAt = key.creationTime.time + validSeconds * 1000L
            check(signatureTime.time <= expiresAt) { "PGP signature was made after the key expired" }
        }
        check(signatureTime.time <= System.currentTimeMillis() + 10L * 60 * 1000) {
            "PGP signature time is in the future"
        }
    }

    @Suppress("DEPRECATION")
    private fun verifyApk(context: Context, requestedRun: CiRun, apk: File) {
        val packageManager = context.packageManager
        val archiveInfo = getPackageInfo(packageManager, apk.absolutePath)
            ?: error("Android rejected the APK signature or manifest")
        check(archiveInfo.packageName == context.packageName) { "APK package name does not match" }

        val currentInfo = getPackageInfo(packageManager, context.packageName)
            ?: error("Cannot read the installed package")
        val newVersion = PackageInfoCompat.getLongVersionCode(archiveInfo)
        val currentVersion = PackageInfoCompat.getLongVersionCode(currentInfo)
        check(newVersion > currentVersion) {
            "APK version $newVersion is not newer than installed version $currentVersion"
        }
        check(newVersion >= requestedRun.versionCode) {
            "APK version $newVersion is older than requested version ${requestedRun.versionCode}"
        }

        val apkRunId = archiveInfo.applicationInfo?.metaData
            ?.getString(CI_RUN_ID_META_DATA)
            ?.removePrefix("run-")
            ?.toLongOrNull()
            ?: error("APK does not contain a valid CI run ID")
        check(apkRunId >= requestedRun.runId && apkRunId > BuildConfig.CI_RUN_ID) {
            "APK CI run ID $apkRunId is not the requested update"
        }

        val archiveSigners = currentSigners(archiveInfo)
        val installedSigners = currentSigners(currentInfo)
        check(archiveSigners.isNotEmpty()) { "APK does not have an Android signing certificate" }
        check(archiveSigners == installedSigners) {
            "APK Android signing certificate does not match the installed app"
        }
    }

    @Suppress("DEPRECATION")
    private fun getPackageInfo(packageManager: PackageManager, packageNameOrPath: String): PackageInfo? {
        val flags = PackageManager.GET_META_DATA or if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            PackageManager.GET_SIGNING_CERTIFICATES
        } else {
            PackageManager.GET_SIGNATURES
        }
        return if (File(packageNameOrPath).isFile) {
            packageManager.getPackageArchiveInfo(packageNameOrPath, flags)
        } else {
            packageManager.getPackageInfo(packageNameOrPath, flags)
        }
    }

    @Suppress("DEPRECATION")
    private fun currentSigners(info: PackageInfo): Set<String> {
        val signatures = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.signingInfo?.apkContentsSigners.orEmpty()
        } else {
            info.signatures.orEmpty()
        }
        return signatures.mapTo(mutableSetOf()) { signature ->
            MessageDigest.getInstance("SHA-256").digest(signature.toByteArray()).toHex()
        }
    }

    private fun copyWithLimit(
        input: InputStream,
        output: java.io.OutputStream,
        limit: Long,
    ) {
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var total = 0L
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            check(total <= limit) { "Downloaded file exceeds its size limit" }
            output.write(buffer, 0, count)
        }
    }

    private suspend fun copyDownloadWithProgress(
        input: InputStream,
        output: java.io.OutputStream,
        contentLength: Long,
        onProgress: (Int) -> Unit,
    ) {
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        var total = 0L
        var lastProgress = -1
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            check(total <= MAX_ARCHIVE_BYTES) { "Downloaded file exceeds its size limit" }
            output.write(buffer, 0, count)
            if (contentLength > 0) {
                val progress = ((total * 100L) / contentLength).toInt().coerceIn(0, 100)
                if (progress != lastProgress) {
                    lastProgress = progress
                    withContext(Dispatchers.Main) { onProgress(progress) }
                }
            }
        }
    }

    private fun fingerprint(key: PGPPublicKey): String = key.fingerprint.toHex()

    private fun ByteArray.toHex(): String = joinToString("") { "%02X".format(it) }

    private fun shellQuote(value: String): String = "'" + value.replace("'", "'\\''") + "'"

    private fun updateSignatureSibling(apk: File): File = File(apk.parentFile, SIGNATURE_NAME)
}
