package com.gow3.android.game;

import android.content.Context;
import com.gow3.android.data.Gow3Data;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.function.Consumer;

/** Installs the signed APK's Linux runtime transactionally in this app's private data directory. */
final class RuntimeInstaller {
    private static void deleteTree(File root) throws Exception {
        if (!root.exists()) return;
        Files.walkFileTree(root.toPath(), new SimpleFileVisitor<java.nio.file.Path>() {
            @Override public FileVisitResult visitFile(java.nio.file.Path path, BasicFileAttributes attrs)
                    throws java.io.IOException {
                Files.delete(path);
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(java.nio.file.Path path, java.io.IOException error)
                    throws java.io.IOException {
                if (error != null) throw error;
                Files.delete(path);
                return FileVisitResult.CONTINUE;
            }
        });
    }

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
        return out.toByteArray();
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format("%02x", b & 255));
        return out.toString();
    }

    private static File path(File root, String relative) { return new File(root, relative); }

    private static void requireExecutable(File root, String relative) throws Exception {
        File file = path(root, relative);
        if (!file.isFile() || !file.canExecute())
            throw new IllegalStateException("Runtime archive is incomplete: executable " + relative);
    }

    private static void requireDirectory(File root, String relative) throws Exception {
        File directory = path(root, relative);
        if (!directory.isDirectory())
            throw new IllegalStateException("Runtime archive is incomplete: directory " + relative);
    }

    /** @param applicationId the running app's id, so a payload built for another one is refused
     *  instead of silently writing into filesDir paths the payload's manifest disagrees with. */
    static void validateRuntime(File root, String applicationId) throws Exception {
        // debug 变体的包名带 ".debug" 后缀（build.gradle.kts applicationIdSuffix），payload
        // 仍按基础 id（com.gow3.android.bionic）发布且同源同 sha256，因此比较前剥掉后缀。
        String manifestId = applicationId.endsWith(".debug")
                ? applicationId.substring(0, applicationId.length() - ".debug".length())
                : applicationId;
        File manifest = path(root, "runtime-manifest.json");
        if (!manifest.isFile()) throw new IllegalStateException("Runtime archive is incomplete: manifest");
        String text = new String(Files.readAllBytes(manifest.toPath()), StandardCharsets.UTF_8);
        if (!text.contains("\"schema_version\": 1") ||
                !text.contains("\"application_id\": \"" + manifestId + "\"") ||
                !text.contains("\"java_package\": \"com.gow3.android.game\""))
            throw new IllegalStateException("Runtime archive is incompatible: manifest identity");
        if (!path(root, "gow3port/bionic/bin/gow3-probe").isFile()) {
            // Bionic 是这个 app 唯一能启动的 runtime（GameActivity 只有 run-bionic.sh 一条启动路）。
            // 一个 glibc/rootfs 形状的 payload 即使完整，装上也跑不了，所以必须在这里拒绝，
            // 而不是像以前那样回落去校验 glibc 布局然后把它装进设备。
            throw new IllegalStateException(
                    "Runtime archive is not a Bionic runtime (missing gow3port/bionic/bin/gow3-probe)");
        }
        requireExecutable(root, "gow3port/bionic/bin/gow3-probe");
        requireExecutable(root, "gow3port/bionic/bin/kgsl-chip-id");
        requireExecutable(root, "gow3port/run-bionic.sh");
        requireDirectory(root, "home/gow3port");
        if (path(root, "bbport/rootfs").exists() || path(root, "gow3port/rootfs").exists())
            throw new IllegalStateException("Runtime archive has an invalid nested rootfs layout");
        if (!path(root, "gow3port/bionic/lib/libgow3cpu.so").isFile() ||
                !path(root, "gow3port/bionic/lib/libgow3gpu.so").isFile())
            throw new IllegalStateException("Runtime archive is incomplete: Bionic ARM64 libraries");
    }

    private static void renameRequired(File source, File target, String label) throws Exception {
        if (!source.exists()) return;
        if (!source.renameTo(target)) throw new IllegalStateException("Cannot stage old " + label);
    }

    static void promote(File files, File stage, String expected) throws Exception {
        File previous = new File(files, ".runtime-previous");
        deleteTree(previous);
        if (!previous.mkdirs()) throw new IllegalStateException("Cannot create runtime rollback directory");
        File oldRoot = new File(previous, "rootfs");
        File oldPort = new File(previous, "gow3port");
        File oldHome = new File(previous, "home");
        File oldManifest = new File(previous, "runtime-manifest.json");
        File root = new File(files, "rootfs");
        File port = new File(files, "gow3port");
        File home = new File(files, "home");
        File manifest = new File(files, "runtime-manifest.json");
        // Bionic 运行时没有 rootfs：stage 里不存在就不提升新的（旧的 glibc rootfs 仍被清掉）。
        boolean stagedRoot = new File(stage, "rootfs").exists();
        boolean movedRoot = false, movedPort = false, movedHome = false, movedManifest = false;
        boolean installedRoot = false, installedPort = false, installedHome = false, installedManifest = false;
        // 玩家自备的 Lossless.dll（Home 页导入/Steam 下载）放在 gow3port/ 内，但不属于
        // runtime payload——替换 runtime 树时必须保全它，否则一次升级就吃掉玩家的文件
        // （change add-native-frame-generation 的供给契约）。
        File dllKeep = new File(files, ".lsfg-dll-keep");
        deleteTree(dllKeep);
        File oldDll = new File(port, "Lossless.dll");
        final boolean keptDll = oldDll.isFile();
        if (keptDll && !oldDll.renameTo(dllKeep))
            throw new IllegalStateException("Cannot preserve Lossless.dll");
        try {
            if (root.exists()) { renameRequired(root, oldRoot, "rootfs"); movedRoot = true; }
            if (port.exists()) { renameRequired(port, oldPort, "gow3port"); movedPort = true; }
            if (home.exists()) { renameRequired(home, oldHome, "home"); movedHome = true; }
            if (manifest.exists()) { renameRequired(manifest, oldManifest, "manifest"); movedManifest = true; }
            if (stagedRoot) {
                if (!new File(stage, "rootfs").renameTo(root))
                    throw new IllegalStateException("Cannot promote runtime rootfs");
                installedRoot = true;
            }
            if (!new File(stage, "gow3port").renameTo(port))
                throw new IllegalStateException("Cannot promote gow3port");
            installedPort = true;
            if (!new File(stage, "home").renameTo(home))
                throw new IllegalStateException("Cannot promote runtime home");
            installedHome = true;
            if (!new File(stage, "runtime-manifest.json").renameTo(manifest))
                throw new IllegalStateException("Cannot promote runtime manifest");
            installedManifest = true;
            File marker = new File(files, ".runtime.sha256");
            File markerPart = new File(files, ".runtime.sha256.part");
            Files.write(markerPart.toPath(), (expected + "\n").getBytes(StandardCharsets.US_ASCII));
            if (marker.exists() && !marker.delete()) throw new IllegalStateException("Cannot replace runtime marker");
            if (!markerPart.renameTo(marker)) throw new IllegalStateException("Cannot publish runtime marker");
            if (keptDll && !new File(port, "Lossless.dll").exists()) {
                if (!dllKeep.renameTo(new File(port, "Lossless.dll")))
                    throw new IllegalStateException("Cannot restore Lossless.dll");
            }
            deleteTree(dllKeep);
            deleteTree(previous);
        } catch (Exception error) {
            if (installedManifest) deleteTree(manifest);
            if (installedPort) deleteTree(port);
            if (installedRoot) deleteTree(root);
            if (installedHome) deleteTree(home);
            if (movedManifest && oldManifest.exists()) oldManifest.renameTo(manifest);
            if (movedPort && oldPort.exists()) {
                oldPort.renameTo(port);
                if (keptDll && dllKeep.isFile() && !new File(port, "Lossless.dll").exists())
                    dllKeep.renameTo(new File(port, "Lossless.dll"));
            }
            if (movedRoot && oldRoot.exists()) oldRoot.renameTo(root);
            if (movedHome && oldHome.exists()) oldHome.renameTo(home);
            throw error;
        }
    }

    static boolean ensure(Context context, File files, Consumer<String> status) throws Exception {
        String expected;
        try (InputStream in = context.getAssets().open("runtime.sha256")) {
            expected = new String(readAll(in), StandardCharsets.US_ASCII).trim();
        } catch (java.io.FileNotFoundException missing) {
            try {
                validateRuntime(files, context.getPackageName());
                Gow3Data.prepare(files, path(files, "gow3port/gow3.default.ini"));
                return false;
            }
            catch (Exception incomplete) {
                throw new IllegalStateException("This APK does not contain a complete runtime", missing);
            }
        }
        if (!expected.matches("[0-9a-f]{64}")) throw new IllegalStateException("Invalid runtime digest");
        File marker = new File(files, ".runtime.sha256");
        if (marker.isFile() && expected.equals(new String(Files.readAllBytes(marker.toPath()), StandardCharsets.US_ASCII).trim())) {
            try {
                validateRuntime(files, context.getPackageName());
                Gow3Data.prepare(files, path(files, "gow3port/gow3.default.ini"));
                return false;
            }
            catch (Exception ignored) { /* stale marker: reinstall from the signed asset */ }
        }

        status.accept("Installing Uncharted runtime…");
        File archive = new File(files, "runtime.tar.gz.part");
        File stage = new File(files, ".runtime-staging-" + expected);
        deleteTree(stage);
        if (!stage.mkdirs()) throw new IllegalStateException("Cannot create runtime staging directory");
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        try {
            try (InputStream in = context.getAssets().open("runtime.payload");
                 FileOutputStream out = new FileOutputStream(archive)) {
                byte[] buffer = new byte[1024 * 1024];
                int count;
                while ((count = in.read(buffer)) != -1) { sha.update(buffer, 0, count); out.write(buffer, 0, count); }
                out.getFD().sync();
            }
            if (!expected.equals(hex(sha.digest())))
                throw new IllegalStateException("Runtime archive checksum mismatch");
            Process tar = new ProcessBuilder("/system/bin/tar", "-oxzf", archive.getAbsolutePath(),
                    "-C", stage.getAbsolutePath()).redirectErrorStream(true).start();
            byte[] log;
            try (InputStream in = tar.getInputStream()) { log = readAll(in); }
            if (tar.waitFor() != 0)
                throw new IllegalStateException("Runtime extraction failed: " +
                        new String(log, 0, Math.min(log.length, 2048), StandardCharsets.UTF_8));
            validateRuntime(stage, context.getPackageName());
            Gow3Data.prepare(files, path(stage, "gow3port/gow3.default.ini"));
            promote(files, stage, expected);
            return true;
        } finally {
            archive.delete();
            deleteTree(stage);
        }
    }

    private RuntimeInstaller() {}
}
