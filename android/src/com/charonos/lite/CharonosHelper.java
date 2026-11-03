package com.charonos.lite;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
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
import java.net.HttpURLConnection;
import java.net.URL;

// Small bridge for the native code (Platform.cpp): storage access, HTTPS and opening files.
public class CharonosHelper {

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
