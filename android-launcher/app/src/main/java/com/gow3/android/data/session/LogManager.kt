package com.gow3.android.data.session

import com.gow3.android.data.Gow3Data
import com.gow3.android.data.archive.LogArchive
import com.gow3.android.data.model.DeviceInfo


import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import com.gow3.android.R
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale
import java.util.concurrent.TimeUnit

/** 一次日志导出尝试的结果。Cancelled 表示用户在文件选择器中退出了。 */
internal sealed class LogResult {
    data class Success(val message: String) : LogResult()
    object Cancelled : LogResult()
    data class Failed(val message: String) : LogResult()
}

/**
 * 负责 Uncharted 运行时日志：把维护者诊断 zip（`report.txt` 设备清单、`android-game.log`
 * 与 `android-game.prev.log` 尾段、`gow3.ini` 设置（去敏感键）、本应用 uid 的 `logcat.txt`）
 * 导出为玩家通过 Storage Access Framework 选择的 `.zip`。参考 Bloodborne-Android 的 LogReport
 * 内容模型；只读取 `filesDir/gow3data` 下的日志/设置与只读系统信息，运行时、runtime 安装、
 * 存档和游戏文件绝不被修改。导出是只读快照，因此导出期间运行游戏只会捕获当时已刷盘的字节。
 */
internal class LogManager(private val activity: Activity) {

    companion object {
        /** 区别于 GameFolderManager 的 1001/1002、LosslessDllManager 的 1003 和 SaveManager 的 1004/1005。 */
        const val REQUEST_EXPORT = 1006
        private const val PROFILE_CAP = 24 shl 20 // profile.txt: at most 24 MB in the export
        val DEFAULT_EXPORT_NAME = "Uncharted-Logs"
    }

    /** 由 [launchExport] 设置，由 [onActivityResult] 消费。 */
    private var pending: ((LogResult) -> Unit)? = null

    /** launcher 的运行时日志目录，即 GameActivity 写入 `android-game.log` 的目录。 */
    fun logsDir(): File = Gow3Data.logs(activity)

    /**
     * 打开系统“另存为”选择器。当没有日志时，立即以失败调用 [onResult]，且不显示
     * 选择器。其他情况下，结果通过 activity 的 legacy 回调经由 [onActivityResult] 到达。
     */
    fun launchExport(onResult: (LogResult) -> Unit) {
        if (!LogArchive.hasLogs(logsDir())) {
            onResult(LogResult.Failed(activity.getString(R.string.log_no_logs)))
            return
        }
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            putExtra(Intent.EXTRA_TITLE, "$DEFAULT_EXPORT_NAME-${LogArchive.stamp()}.zip")
        }
        pending = onResult
        try {
            activity.startActivityForResult(intent, REQUEST_EXPORT)
        } catch (ex: Exception) {
            pending = null
            onResult(LogResult.Failed(activity.getString(R.string.no_file_picker_fmt, ex.message ?: activity.getString(R.string.unknown_error))))
        }
    }

    /** @return 当请求属于我们且已被消费时返回 true。 */
    fun onActivityResult(requestCode: Int, resultCode: Int, intent: Intent?): Boolean {
        if (requestCode != REQUEST_EXPORT) return false
        val callback = pending
        pending = null
        if (callback == null) return true
        val uri = intent?.data
        if (resultCode != Activity.RESULT_OK || uri == null) {
            callback(LogResult.Cancelled)
            return true
        }
        Thread {
            val result = exportTo(uri)
            activity.runOnUiThread { callback(result) }
        }.start()
        return true
    }

    /**
     * 先把诊断 zip 打包成临时文件（SAF 流不可 seek），再复制到选择器返回的 URI。
     * 无论成功或失败，临时文件始终会被删除。
     */
    private fun exportTo(uri: Uri): LogResult {
        val logs = logsDir()
        if (!LogArchive.hasLogs(logs)) return LogResult.Failed(activity.getString(R.string.log_no_logs))
        val temporary = File(Gow3Data.root(activity), ".export-logs.tmp")
        temporary.delete()
        return try {
            LogArchive.writeZip(buildEntries(logs), temporary)
            val output = activity.contentResolver.openOutputStream(uri)
                ?: return LogResult.Failed(activity.getString(R.string.log_write_failed))
            output.use { out ->
                FileInputStream(temporary).use { it.copyTo(out) }
            }
            LogResult.Success(activity.getString(R.string.log_exported_fmt, displayName(uri) ?: activity.getString(R.string.selected_location)))
        } catch (ex: Exception) {
            LogResult.Failed(activity.getString(R.string.log_export_failed_fmt, ex.message ?: activity.getString(R.string.unknown_error)))
        } finally {
            temporary.delete()
        }
    }

    /** 组装诊断 zip 的有序条目；只读，不修改任何既有文件。 */
    private fun buildEntries(logs: File): Map<String, ByteArray> {
        val entries = LinkedHashMap<String, ByteArray>()
        val notes = mutableListOf<String>()
        var secretRemoved = 0

        // 游戏日志尾段（整行截断 + 敏感行脱敏），含上一轮保留的 .prev.log。
        for (name in listOf("android-game.log", "android-game.prev.log")) {
            val file = File(logs, name)
            if (file.isFile) {
                val text = StringWriter().also { secretRemoved += LogArchive.tailStripped(file, LogArchive.LOG_CAP, it) }.toString()
                entries["logs/$name"] = text.toByteArray(Charsets.UTF_8)
                notes += "$name: included (last ${LogArchive.LOG_CAP shr 20} MB)"
            }
        }

        // uncharted_android: the sampling profiler's output (GOW3_PROFILE; gpu/shim/gow3_profiler.cpp),
        // addresses and thread names only, when the player ran a profiling session.
        val profile = File(logs, "profile.txt")
        if (profile.isFile && profile.length() > 0) {
            val bytes = profile.readBytes()
            entries["logs/profile.txt"] = if (bytes.size <= PROFILE_CAP) bytes else bytes.copyOfRange(0, PROFILE_CAP)
            notes += "profile.txt: included (${bytes.size / 1024} KB" + (if (bytes.size > PROFILE_CAP) ", truncated" else "") + ")"
        }

        // gow3.ini 设置（剔除键名疑似敏感的键值对）。
        val ini = Gow3Data.config(activity)
        if (ini.isFile) {
            val (content, skipped) = LogArchive.settingsStripped(ini)
            entries["gow3.ini"] = content.toByteArray(Charsets.UTF_8)
            notes += "Settings: gow3.ini" + if (skipped > 0) " ($skipped lines with secret-looking keys left out)" else ""
        } else {
            notes += "Settings: no gow3.ini"
        }

        // 本应用 uid 的 logcat（整行尾段 + 脱敏），失败降级。
        val logcat = captureLogcat()
        entries["logcat.txt"] = logcat.content.toByteArray(Charsets.UTF_8)
        notes += "Logcat: ${logcat.note}"
        secretRemoved += logcat.removed

        // report.txt：设备清单 + 各条目说明 + 脱敏计数 + 导出时间。
        val report = StringBuilder(DeviceInfo.report(activity))
        notes.forEach { report.append(it).append('\n') }
        if (secretRemoved > 0) report.append("Log lines left out as possible secrets: $secretRemoved\n")
        report.append("Report time: ").append(SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(Date())).append('\n')
        entries["report.txt"] = report.toString().toByteArray(Charsets.UTF_8)

        return entries
    }

    /** 本应用 uid 的 logcat 尾段；无 READ_LOGS 时 Android 只暴露本 uid 行。失败时降级写失败说明。 */
    private fun captureLogcat(): Logcat {
        val lines = ArrayDeque<String>()
        var bytes = 0L
        var cut = false
        var removed = 0
        var note: String
        var process: Process? = null
        try {
            process = ProcessBuilder("logcat", "-d", "-v", "threadtime").redirectErrorStream(true).start()
            process.inputStream.bufferedReader(Charsets.UTF_8).use { reader ->
                for (line in reader.lineSequence()) {
                    if (LogArchive.secretLine.containsMatchIn(line)) {
                        removed++
                        continue
                    }
                    lines.addLast(line)
                    bytes += line.length + 1L
                    while (bytes > LogArchive.LOGCAT_CAP && lines.size > 1) {
                        bytes -= lines.pollFirst().length + 1L
                        cut = true
                    }
                }
            }
            if (!process.waitFor(5, TimeUnit.SECONDS)) throw IOException("logcat did not finish")
            note = "logcat.txt, ${lines.size} lines" + if (cut) " (cut to the last ${LogArchive.LOGCAT_CAP shr 20} MB)" else ""
        } catch (e: Exception) {
            note = "could not be read ($e)"
            lines.clear()
            lines.add("logcat could not be read: $e")
        } finally {
            process?.destroyForcibly()
        }
        val content = buildString {
            if (cut) append("[Cut: only the last ${LogArchive.LOGCAT_CAP shr 20} MB of logcat is kept]\n")
            lines.forEach { append(it).append('\n') }
        }
        return Logcat(content, note, removed)
    }

    private data class Logcat(val content: String, val note: String, val removed: Int)

    /** Provider 的显示名称，回退到 URI 的最后一段。 */
    private fun displayName(uri: Uri): String? {
        try {
            activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (column >= 0 && cursor.moveToFirst()) {
                    val name = cursor.getString(column)
                    if (!name.isNullOrEmpty()) return name
                }
            }
        } catch (_: Exception) {
            // 回退到 URI 段。
        }
        val segment = uri.lastPathSegment ?: return null
        return segment.substringAfterLast('/').takeIf { it.isNotEmpty() }
    }
}
