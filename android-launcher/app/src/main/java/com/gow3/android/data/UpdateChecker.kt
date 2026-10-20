package com.gow3.android.data

import com.gow3.android.data.model.ProjectRepo


import com.gow3.android.R
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import org.json.JSONObject

/**
 * Android 发布的版本串：Uncharted 的 PC 上游版本号（点分数字）加发布序号。
 *
 * GitHub 上表现为 tag，APK 内表现为去掉 `v` 的同一串 `versionName`
 * （`tools/android/release_local.sh` 的 `version=${android_tag#v}`）。序号由
 * `next_android_tag` 在目标仓库已有 tag 上取最大值加一，因此单调递增。
 * bionic 管线起 tag 形态为 `v<PC版本>-bionic-v<N>`（如 `v2.1.7-bionic-v1`）；
 * 旧的 `v<PC版本>-android<N>`（如 `v2.1.6-android3`）仍需解析，用于比较历史 release。
 * 该形态是发布流水线的契约：发布脚本一旦改用别的 tag 形态，本类必须同步。
 *
 * 解析刻意要求整串匹配（[Regex.matchEntire]）：像 `0.5.2-kotlin`（debug 占位版本）或
 * `v2.1.6-android3-rc1` 这样的串解析失败并落到「无法比较」，而不是被误判成可比较版本。
 */
internal data class AndroidReleaseVersion(
    val upstream: List<Long>,
    val androidRevision: Long,
    val bionic: Boolean = false,
) : Comparable<AndroidReleaseVersion> {

    /** 先逐段按数字比上游版本号（位数不足补 0），相等后再比 Android 发布序号。 */
    override fun compareTo(other: AndroidReleaseVersion): Int {
        for (index in 0 until maxOf(upstream.size, other.upstream.size)) {
            val mine = upstream.getOrElse(index) { 0L }
            val theirs = other.upstream.getOrElse(index) { 0L }
            if (mine != theirs) return mine.compareTo(theirs)
        }
        return androidRevision.compareTo(other.androidRevision)
    }

    /** 规范往返形态：android 家族为 `-android<N>`，bionic 家族为 `-bionic-v<N>`。 */
    override fun toString(): String =
        upstream.joinToString(".") + if (bionic) "-bionic-v$androidRevision" else "-android$androidRevision"

    companion object {
        private val PATTERN = Regex("v?(\\d+(?:\\.\\d+)*)-(android|bionic)(?:-v)?(\\d+)")

        /** 解析 release tag 或 `versionName`；形态不符时返回 null 而不是抛异常。 */
        fun parse(raw: String?): AndroidReleaseVersion? {
            val match = PATTERN.matchEntire(raw?.trim().orEmpty()) ?: return null
            val upstream = match.groupValues[1].split('.').map { segment ->
                segment.toLongOrNull() ?: return null
            }
            val revision = match.groupValues[3].toLongOrNull() ?: return null
            return AndroidReleaseVersion(upstream, revision, bionic = match.groupValues[2] == "bionic")
        }
    }
}

/** GitHub 最新 release 中检查需要的字段。 */
internal data class GitHubRelease(
    val tag: String,
    val pageUrl: String,
    val apkUrl: String?,
)

/** 「无法比较」的原因，用于选择不同的文案。 */
internal enum class NotComparableReason {
    /** APK 自身的 `versionName` 不是 Android 发布版本串，例如 debug 占位值。 */
    INSTALLED_VERSION,

    /** 仓库最新 release 的 tag 不是 Android 发布形态。 */
    LATEST_TAG,
}

/**
 * 一次检测更新的结果。检查失败以 [Failed] 呈现而不是抛出异常，
 * 让 About 页只需要穷举分支、不必处理异常。
 */
internal sealed class UpdateCheckResult {
    data class UpToDate(val version: String) : UpdateCheckResult()

    data class Newer(val version: String, val release: GitHubRelease) : UpdateCheckResult()

    data class NotComparable(
        val currentVersion: String?,
        val latestTag: String?,
        val reason: NotComparableReason,
    ) : UpdateCheckResult()

    /** [messageRes] 是面向玩家的本地化文案；失败细节一律保留在日志或详情里。 */
    data class Failed(val messageRes: Int) : UpdateCheckResult()
}

/**
 * 对照本项目 GitHub 仓库的最新 release 判断 APK 是否需要更新。
 *
 * launcher 不下载也不安装 APK，只提示玩家并跳转到浏览器中的下载页，因此本组件不触碰
 * 文件系统、runtime 与游戏数据。
 */
internal object UpdateChecker {

    /**
     * 纯比较：把已取到的最新 release 与当前版本比成结果，不涉及网络。
     *
     * [currentVersion] 与 [latestTag] 任一无法解析时一律返回 [UpdateCheckResult.NotComparable]，
     * 绝不返回 [UpdateCheckResult.UpToDate] 或 [UpdateCheckResult.Newer] —— 避免在无法判断时
     * 对玩家给出错误的结论。
     */
    fun compare(currentVersion: String?, latestTag: String?, release: GitHubRelease?): UpdateCheckResult {
        val current = AndroidReleaseVersion.parse(currentVersion)
            ?: return UpdateCheckResult.NotComparable(currentVersion, latestTag, NotComparableReason.INSTALLED_VERSION)
        val latest = AndroidReleaseVersion.parse(latestTag)
            ?: return UpdateCheckResult.NotComparable(currentVersion, latestTag, NotComparableReason.LATEST_TAG)
        if (latest <= current) return UpdateCheckResult.UpToDate(latest.toString())
        return UpdateCheckResult.Newer(latest.toString(), release ?: GitHubRelease(latestTag.orEmpty(), "", null))
    }

    /**
     * 取本项目仓库的最新 release 并与 [currentVersion] 比较。
     *
     * 只发起一次只读 GET，不下载也不安装任何文件；任何失败都折叠成
     * [UpdateCheckResult.Failed] 并带上本地化文案，绝不把异常抛给调用方。
     * 必须在后台线程（`Dispatchers.IO`）上调用。
     */
    fun check(currentVersion: String): UpdateCheckResult {
        val connection = try {
            (URL(ProjectRepo.LATEST_RELEASE_API).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = CONNECT_TIMEOUT_MS
                readTimeout = READ_TIMEOUT_MS
                setRequestProperty("Accept", "application/vnd.github+json")
                // GitHub 拒绝没有 User-Agent 的请求，同时它也是未认证限额的依据。
                setRequestProperty("User-Agent", "gow3-android-launcher")
            }
        } catch (error: Exception) {
            return UpdateCheckResult.Failed(R.string.update_check_failed_network)
        }

        val body = try {
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            if (status !in 200..299) return UpdateCheckResult.Failed(failureFor(status, text, connection))
            text
        } catch (error: SocketTimeoutException) {
            return UpdateCheckResult.Failed(R.string.update_check_failed_timeout)
        } catch (error: Exception) {
            return UpdateCheckResult.Failed(R.string.update_check_failed_network)
        } finally {
            connection.disconnect()
        }

        val latest = try {
            val json = JSONObject(body)
            readRelease(json)
        } catch (error: Exception) {
            null
        } ?: return UpdateCheckResult.Failed(R.string.update_check_failed_response)

        return compare(currentVersion, latest.tag, latest)
    }

    /** 从 release JSON 中取出比较与跳转需要的字段；结构不符时返回 null。 */
    private fun readRelease(json: JSONObject): GitHubRelease? {
        val tag = json.optString(TAG_NAME).takeIf { it.isNotBlank() } ?: return null
        val page = json.optString(HTML_URL).takeIf { it.isNotBlank() }.orEmpty()
        val assets = json.optJSONArray(ASSETS)
        var apk: String? = null
        for (index in 0 until (assets?.length() ?: 0)) {
            val asset = assets!!.optJSONObject(index) ?: continue
            // 发布产物的 APK 名含点号（如 God.of.War.III-2.1.6-android3.apk），按后缀匹配即可。
            if (!asset.optString(NAME).endsWith(APK_SUFFIX, ignoreCase = true)) continue
            apk = asset.optString(BROWSER_DOWNLOAD_URL).takeIf { it.isNotBlank() }
            if (apk != null) break
        }
        return GitHubRelease(tag, page, apk ?: page.takeIf { it.isNotBlank() })
    }

    /** 把 HTTP 失败区分成速率限额与一般失败，让提示更可读。 */
    private fun failureFor(status: Int, body: String, connection: HttpURLConnection): Int = when {
        status == 403 || status == 429 ->
            if (connection.getHeaderField(RATE_LIMIT_REMAINING) == "0" || body.contains("rate limit", ignoreCase = true)) {
                R.string.update_check_failed_rate_limit
            } else {
                R.string.update_check_failed_http
            }

        else -> R.string.update_check_failed_http
    }

    private const val CONNECT_TIMEOUT_MS = 10_000
    private const val READ_TIMEOUT_MS = 15_000
    private const val TAG_NAME = "tag_name"
    private const val HTML_URL = "html_url"
    private const val ASSETS = "assets"
    private const val NAME = "name"
    private const val BROWSER_DOWNLOAD_URL = "browser_download_url"
    private const val RATE_LIMIT_REMAINING = "x-ratelimit-remaining"
    private const val APK_SUFFIX = ".apk"
}
