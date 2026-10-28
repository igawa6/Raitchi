package com.lasrecomp.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Reads package metadata without preparing native code or starting a game. */
public final class RaitchiMetadata {
    private static final long MAX_KEYS_BYTES = 8L << 20;

    public static final class GameInfo {
        public final String title, developer, programId, programIdHex, version;
        public final String iconPath, adapterId, reason;
        public final boolean compatible;
        /** True only when no adapter matches and the engine accepted generic startup
         *  (adapterId is then "generic"). Never true for a supported adapter. */
        public final boolean tryable;

        private GameInfo(JSONObject value) throws IOException {
            title = required(value, "title");
            developer = value.optString("developer", "");
            programId = required(value, "programId");
            programIdHex = required(value, "programIdHex");
            version = value.optString("version", "");
            iconPath = value.optString("iconPath", "");
            adapterId = value.optString("adapterId", "");
            compatible = value.optBoolean("compatible", false);
            tryable = !compatible && TitlePolicy.generic(adapterId) && value.optBoolean("tryable", false);
            reason = value.optString("reason", "");
        }
    }

    /** Stable private location used by both inspection and package preparation. */
    public static File keysFile(Context context) {
        return new File(new File(context.getApplicationContext().getFilesDir(), "shell"), "prod.keys");
    }

    /** Copies a SAF-selected prod.keys atomically and retains no provider descriptor. */
    public static File importKeys(Context context, Uri source) throws IOException {
        if (source == null) throw new IOException("Select a prod.keys file");
        Context app = context.getApplicationContext();
        try { app.getContentResolver().takePersistableUriPermission(source, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException | UnsupportedOperationException ignored) { }
        File target = keysFile(app), parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) throw new IOException("Cannot create private keys folder");
        File temp = new File(parent, "prod.keys.tmp");
        try (InputStream in = app.getContentResolver().openInputStream(source)) {
            if (in == null) throw new IOException("Keys file is unavailable");
            try (FileOutputStream out = new FileOutputStream(temp, false)) {
                byte[] buffer = new byte[8192]; long total = 0; int n;
                while ((n = in.read(buffer)) != -1) {
                    total += n;
                    if (total > MAX_KEYS_BYTES) throw new IOException("Keys file is too large");
                    out.write(buffer, 0, n);
                }
                out.getFD().sync();
            }
            if (!containsHeaderKey(temp)) throw new IOException("Keys file does not contain a valid header_key");
            if (!temp.renameTo(target)) throw new IOException("Cannot publish private keys file");
            return target;
        } catch (IOException | RuntimeException e) {
            // Keep the prior usable keys file on every failed import.
            //noinspection ResultOfMethodCallIgnored
            temp.delete();
            if (e instanceof IOException) throw (IOException)e;
            throw new IOException("Cannot import keys file", e);
        }
    }

    /** True when the private file is readable and contains the required NCA header key. */
    public static boolean hasKeys(Context context) {
        try { return containsHeaderKey(keysFile(context.getApplicationContext())); }
        catch (IOException e) { return false; }
    }

    public static GameInfo inspect(Context context, Uri packageUri) throws IOException {
        if (packageUri == null) throw new IOException("Select an NSP or XCI file");
        Context app = context.getApplicationContext();
        File keys = keysFile(app);
        if (!containsHeaderKey(keys)) throw new IOException("Import a readable prod.keys file first");
        File icons = new File(app.getCacheDir(), "game-icons");
        if (!icons.isDirectory() && !icons.mkdirs()) throw new IOException("Cannot create game icon cache");
        System.loadLibrary("raitchiprepare");
        try (ParcelFileDescriptor fd = app.getContentResolver().openFileDescriptor(packageUri, "r")) {
            if (fd == null) throw new IOException("Game file is unavailable");
            return new GameInfo(new JSONObject(new String(inspectNative(fd.getFd(), keys.getAbsolutePath(), icons.getAbsolutePath()), StandardCharsets.UTF_8)));
        } catch (org.json.JSONException e) {
            throw new IOException("Native package metadata is invalid", e);
        } catch (SecurityException e) {
            throw new IOException("Game-file permission expired. Select the file again.", e);
        }
    }

    private static native byte[] inspectNative(int fd, String keysPath, String iconCacheDir) throws IOException;

    private static String required(JSONObject value, String key) throws IOException {
        String result = value.optString(key, "");
        if (result.isEmpty()) throw new IOException("Package metadata is missing " + key);
        return result;
    }

    private static boolean containsHeaderKey(File file) throws IOException {
        if (!file.isFile() || file.length() <= 0 || file.length() > MAX_KEYS_BYTES) return false;
        boolean found = false;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.US_ASCII))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int comment = line.indexOf('#'); if (comment >= 0) line = line.substring(0, comment);
                int split = line.indexOf('='); if (split < 0) continue;
                if (!"header_key".equals(line.substring(0, split).trim().toLowerCase(Locale.ROOT))) continue;
                String value = line.substring(split + 1).replaceAll("\\s", "");
                if (value.matches("(?i)[0-9a-f]{64}")) found = true;
            }
        }
        return found;
    }

    private RaitchiMetadata() { }
}
