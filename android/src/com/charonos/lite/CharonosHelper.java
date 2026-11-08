package com.charonos.lite;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.webkit.MimeTypeMap;

import androidx.core.content.FileProvider;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.net.HttpURLConnection;
import java.net.URL;

// Small bridge for the native code (Platform.cpp): storage access, HTTPS and opening files.
public class CharonosHelper {

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    // Run yt-dlp in Termux: Android does not provide an app-local Python runtime.
    // Status, progress and output paths are written in the shared download folder for CHARONOS to poll.
    public static int startYtDlpDownload(Context context, String url, String folder, String token) {
        try {
            context.getPackageManager().getPackageInfo("com.termux", 0);
        } catch (Exception e) {
            return 1;
        }
        if (context.checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
            try {
                Intent settings = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.parse("package:" + context.getPackageName()));
                settings.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(settings);
            } catch (Exception ignored) {}
            return 2;
        }
        try {
            File outputDir = new File(folder);
            if (!outputDir.exists() && !outputDir.mkdirs()) return 3;
            String base = ".charonos-yt-dlp-" + token;
            File scriptFile = new File(outputDir, base + ".sh");
            File statusFile = new File(outputDir, base + ".status");
            File logFile = new File(outputDir, base + ".log");
            statusFile.delete();
            logFile.delete();

            String template = new File(outputDir, "%(title).200B [%(id)s].%(ext)s").getPath();
            String script = "#!/data/data/com.termux/files/usr/bin/bash\n" +
                "yt-dlp --newline --no-warnings --no-playlist " +
                "--progress-template " + shellQuote("download:CHARONOS_PROGRESS:%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s") + " " +
                "--print-to-file " + shellQuote("after_move:CHARONOS_FILE:%(filepath)s") + " " + shellQuote(statusFile.getPath()) + " " +
                "-f " + shellQuote("best[ext=mp4]/best") + " -o " + shellQuote(template) + " " + shellQuote(url) +
                " > " + shellQuote(logFile.getPath()) + " 2>&1\n" +
                "rc=$?\nprintf 'CHARONOS_EXIT:%s\\n' \"$rc\" >> " + shellQuote(statusFile.getPath()) + "\nexit \"$rc\"\n";
            try (OutputStreamWriter writer = new OutputStreamWriter(new FileOutputStream(scriptFile), StandardCharsets.UTF_8)) {
                writer.write(script);
            }

            Intent run = new Intent();
            run.setClassName("com.termux", "com.termux.app.RunCommandService");
            run.setAction("com.termux.RUN_COMMAND");
            run.putExtra("com.termux.RUN_COMMAND_PATH", "/data/data/com.termux/files/usr/bin/bash");
            run.putExtra("com.termux.RUN_COMMAND_ARGUMENTS", new String[]{scriptFile.getPath()});
            run.putExtra("com.termux.RUN_COMMAND_WORKDIR", "/data/data/com.termux/files/home");
            run.putExtra("com.termux.RUN_COMMAND_BACKGROUND", true);
            run.putExtra("com.termux.RUN_COMMAND_LABEL", "CHARONOS video download");
            context.startService(run);
            return 0;
        } catch (Exception e) {
            return 3;
        }
    }

    // Media and projects are read by path (so a project opens the same way on Windows).
    public static boolean hasStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    public static void requestStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= 30) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                       Uri.parse("package:" + context.getPackageName()));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try {
                context.startActivity(intent);
            } catch (Exception e) {
                Intent all = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                all.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(all);
            }
            return;
        }
        if (context instanceof Activity) {
            ((Activity) context).requestPermissions(new String[] {
                Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }, 7301);
        }
    }

    // GET with the platform's TLS (Qt Network has no TLS backend here without OpenSSL).
    public static byte[] httpGet(String address, int timeoutMs) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(address).openConnection();
        try {
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent", "CHARONOS-Lite-Android");
            c.setRequestProperty("Accept", "application/vnd.github+json");
            int code = c.getResponseCode();
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            if (in != null) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                in.close();
            }
            return out.toByteArray();
        } finally {
            c.disconnect();
        }
    }

    // Opens a local file (an exported movie, a downloaded APK) with the app the system picks.
    public static boolean openFile(Context context, String path) {
        try {
            File file = new File(path);
            Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + ".qtprovider", file);
            String ext = MimeTypeMap.getFileExtensionFromUrl(file.getName());
            String mime = path.endsWith(".apk") ? "application/vnd.android.package-archive"
                                                : MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, mime != null ? mime : "*/*");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
