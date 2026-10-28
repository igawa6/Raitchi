package com.lasrecomp.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.database.Cursor;
import android.os.SystemClock;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;

/** Narrow Android-facing bridge from Eden's shell to Raitchi's existing engine launcher. */
public final class RaitchiShellBridge {
    /** Adapter key for trying a game without a Raitchi adapter (GameInfo.tryable). */
    public static final String GENERIC_ADAPTER = TitlePolicy.GENERIC;

    /** Unknown adapters may appear in the library before they can be launched. */
    public static boolean isAdapterExperimental(String adapterId) {
        try { return TitlePolicy.get(adapterId).experimental; }
        catch (IllegalArgumentException unsupported) { return false; }
    }

    /** Whether this adapter has a qualified render-target scaling policy. */
    public static boolean supportsRenderScaling(String adapterId) {
        return TitlePolicy.supportsRenderScaling(adapterId);
    }

    /** Engine-specific option capabilities stay behind the Android engine boundary. */
    public static SettingsStore.Option[] settingOptions(String adapterId) {
        if (adapterId != null) SharedFiles.checked(adapterId);
        List<SettingsStore.Option> result = new ArrayList<>();
        for (SettingsStore.Option option : SettingsStore.OPTIONS) {
            // The compact shell ships the native package path; AOT tools stay in the legacy app.
            if ("cpu_backend".equals(option.key)) continue;
            if (option.lasOnly && adapterId != null && !"las".equals(adapterId)) continue;
            if (TitlePolicy.nativeOnly(adapterId) && "cpu_backend".equals(option.key)) continue;
            result.add(option);
        }
        return result.toArray(new SettingsStore.Option[0]);
    }

    private static final String EMULATION_ACTIVITY = "com.lasrecomp.android.LasActivity";
    private static final AtomicBoolean folderRunning = new AtomicBoolean();
    private static final AtomicBoolean driverInstalling = new AtomicBoolean();
    private static volatile String folderStatus = "";
    private static volatile long preparationPendingUntil;

    public static final class Title {
        public final String id;
        public final String name;
        public final String metadata;
        public final boolean compatible;
        public final boolean imported;
        public final boolean ready;
        public final String status;

        private Title(String id, String name, String metadata, boolean compatible,
                      boolean imported, boolean ready, String status) {
            this.id = id;
            this.name = name;
            this.metadata = metadata;
            this.compatible = compatible;
            this.imported = imported;
            this.ready = ready;
            this.status = status;
        }
    }

    /** Returns only titles with compiled Raitchi adapters. */
    public static List<Title> titles(Context context) {
        Context app = context.getApplicationContext();
        List<Title> result = new ArrayList<>(SharedFiles.TITLES.length);
        for (String id : SharedFiles.TITLES) {
            boolean imported = SharedFiles.imported(app, id);
            boolean ready = SharedFiles.ready(app, id);
            String status;
            if (ready) {
                status = "Ready to play";
            } else if (folderRunning.get()) {
                status = folderStatus;
            } else if (NativePackage.isRunning()) {
                status = NativePackage.status();
            } else if (!preparationStatus(app, id).isEmpty()) {
                status = preparationStatus(app, id);
            } else if (imported) {
                status = "Imported; prepare and verify the supported game version";
            } else {
                status = "Add a supported game file";
            }
            result.add(new Title(id, SharedFiles.label(id), metadata(id), true,
                    imported, ready, status));
        }
        return Collections.unmodifiableList(result);
    }

    private static String metadata(String id) {
        switch (id) {
            case "las":
                return "The Legend of Zelda: Link’s Awakening · supported adapter";
            case "eevee":
                return "Pokémon: Let’s Go, Eevee! · supported adapter";
            case "echoes":
                return "The Legend of Zelda: Echoes of Wisdom · supported adapter";
            case "wonder":
                return "Super Mario Bros. Wonder · experimental base 1.0.0 adapter";
            default:
                throw new IllegalArgumentException("Unsupported title: " + id);
        }
    }

    public static String getSetting(Context context, String title, String key) {
        return new SettingsStore(context.getApplicationContext()).getString(title, key);
    }

    public static void setSetting(Context context, String title, String key, String value) {
        new SettingsStore(context.getApplicationContext()).setString(title, key, value);
    }

    /** Starts Raitchi's existing NSP/XCI preparation worker using persisted SAF documents. */
    public static void beginPackageImport(Context context, String title, Uri gameUri, Uri keysUri)
            throws IOException {
        requireIdle(context);
        if (gameUri == null || keysUri == null) {
            throw new IOException("Select both a game file and a prod.keys file");
        }
        if (!"content".equals(gameUri.getScheme()) || !"content".equals(keysUri.getScheme()))
            throw new IOException("Choose the game file and keys using Android’s file picker");
        NativePackage.start(context, title, gameUri, keysUri);
    }

    public static boolean isPackageImportRunning() {
        return NativePackage.isRunning();
    }

    public static String packageImportStatus() {
        return NativePackage.status();
    }

    /** Prepares the isolated session and returns an explicit intent for SDL's own Activity. */
    public static Intent prepareSession(Context context, String title) throws IOException {
        Context app = context.getApplicationContext();
        SharedFiles.checked(title);
        if (gameRunning(app)) {
            String current = SharedFiles.read(new File(app.getFilesDir(), "game-session.json")).optString("title");
            if (!title.equals(current)) throw new IOException("Quit the running game before switching games");
            return resumeSession(app);
        }
        requireIdle(app);
        SharedFiles.session(app, title);
        String checkedTitle = SharedFiles.checked(title);
        SharedFiles.atomic(new File(app.getFilesDir(), "game-session.json"),
                "{\"title\":\"" + checkedTitle + "\"}");
        return new Intent().setClassName(app, EMULATION_ACTIVITY);
    }

    public static boolean gameRunning(Context context) {
        return SharedLauncherActivity.gamePid(context) != 0;
    }

    public static Intent resumeSession(Context context) throws IOException {
        if (!gameRunning(context)) throw new IOException("No game is running; select Play from the library");
        String title = SharedFiles.read(new File(context.getFilesDir(), "game-session.json")).optString("title");
        try { SharedFiles.checked(title); }
        catch (IllegalArgumentException error) { throw new IOException("The running session has no supported title", error); }
        return new Intent().setClassName(context, EMULATION_ACTIVITY)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
    }

    public static boolean isPreparationRunning(Context context) {
        return SystemClock.elapsedRealtime() < preparationPendingUntil ||
                (SharedLauncherActivity.processPid(context, ":build") != 0 &&
                SharedFiles.read(new File(context.getFilesDir(), "builds/active.json")).optBoolean("running"));
    }

    public static String preparationStatus(Context context, String title) {
        SharedFiles.checked(title);
        return SharedFiles.buildStatus(SharedFiles.read(SharedFiles.progress(context, title)),
                SharedFiles.read(new File(context.getFilesDir(), "builds/active.json")), title);
    }

    public static void beginPreparation(Context context, String title) throws IOException {
        requireIdle(context);
        SharedFiles.checked(title);
        if (TitlePolicy.nativeOnly(title)) throw new IOException("Import an NSP/XCI for this native-only title");
        if (!SharedFiles.extractedImported(context, title)) throw new IOException("Import an extracted game folder first");
        preparationPendingUntil = SystemClock.elapsedRealtime() + 3000;
        try {
            context.startForegroundService(new Intent(context, SharedBuildService.class).putExtra("title", title));
        } catch (RuntimeException error) {
            preparationPendingUntil = 0;
            throw new IOException("Cannot start game preparation", error);
        }
    }

    public static void cancelPreparation(Context context, String title) throws IOException {
        SharedFiles.checked(title);
        SharedFiles.atomic(SharedFiles.cancel(context, title), "cancel\n");
    }

    public static boolean isBusy(Context context) {
        return folderRunning.get() || driverInstalling.get() || NativePackage.isRunning() || RaitchiUiDriverImport.isRunning() ||
                isPreparationRunning(context) || gameRunning(context);
    }

    private static void requireIdle(Context context) throws IOException {
        if (isBusy(context)) throw new IOException("Finish the current task or quit the running game first");
    }

    public static boolean isFolderImportRunning() { return folderRunning.get(); }
    public static String folderImportStatus() { return folderStatus; }

    /** Copies only executable/assets trees into an owned stage; never overwrites an existing import. */
    public static void importExtractedFolder(Context owner, String title, Uri tree) throws IOException {
        requireIdle(owner);
        SharedFiles.checked(title);
        if (TitlePolicy.nativeOnly(title)) throw new IOException("Import an NSP/XCI for this native-only title");
        if (tree == null || !"content".equals(tree.getScheme()) || !DocumentsContract.isTreeUri(tree))
            throw new IOException("Choose an extracted game folder using Android’s folder picker");
        Context context = owner.getApplicationContext();
        if (SharedFiles.extractedImported(context, title) || NativePackage.selection(context, title) != null)
            throw new IOException("This game is already imported; the existing game files were kept");
        if (!folderRunning.compareAndSet(false, true)) throw new IOException("A folder import is already running");
        folderStatus = "Importing " + SharedFiles.label(title) + "… Keep Raitchi open";
        Thread worker = new Thread(() -> {
            File stage = null;
            try {
                // Keep a partial legacy import for recovery while allowing a new, complete one.
                // Only the fixed private extracted directory can be moved; saves live elsewhere.
                File old = SharedFiles.game(context, title);
                if (old.exists() || Files.isSymbolicLink(old.toPath())) {
                    if (Files.isSymbolicLink(old.toPath()) || !old.getCanonicalFile().equals(old.getAbsoluteFile()))
                        throw new IOException("Unsafe existing game folder");
                    File preserved = new File(old.getParentFile(), ".incomplete-" + UUID.randomUUID());
                    if (!old.renameTo(preserved)) throw new IOException("Cannot preserve the incomplete import");
                }
                stage = RaitchiUiPackageStaging.begin(context.getFilesDir(), title);
                copyTree(context, tree, DocumentsContract.getTreeDocumentId(tree), stage, true, new long[]{0}, 0);
                if (!new File(stage, "exefs/main").isFile() || !new File(stage, "exefs/sdk").isFile() ||
                        (!"las".equals(title) && (!new File(stage, "exefs/subsdk0").isFile() ||
                        !new File(stage, "exefs/subsdk1").isFile())) ||
                        (!new File(stage, "romfs.bin").isFile() && !new File(stage, "romfs").isDirectory()))
                    throw new IOException("Select the folder containing exefs and romfs (or romfs.bin)");
                RaitchiUiPackageStaging.publish(context.getFilesDir(), title, stage);
                stage = null;
                folderStatus = "Game files imported. Select Prepare game to verify and prepare them";
            } catch (Exception error) {
                folderStatus = "Import stopped: " + error.getMessage();
            } finally {
                RaitchiUiPackageStaging.discard(context.getFilesDir(), title, stage);
                folderRunning.set(false);
            }
        }, "raitchi-shell-folder-import");
        try { worker.start(); } catch (RuntimeException | Error error) { folderRunning.set(false); throw error; }
    }

    private static void copyTree(Context c, Uri tree, String id, File destination, boolean root,
                                 long[] copied, int depth) throws IOException {
        if (depth > 64) throw new IOException("Game folder is nested too deeply");
        if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Cannot create import folder");
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor cursor = c.getContentResolver().query(
                DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), columns, null, null, null)) {
            if (cursor == null) throw new IOException("Folder is unavailable");
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                        name.contains("/") || name.contains("\\")) throw new IOException("Invalid game file name");
                if (root && !name.equals("exefs") && !name.equals("romfs") && !name.equals("romfs.bin")) continue;
                File file = new File(destination, name);
                if (file.exists()) throw new IOException("Duplicate game file: " + name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    copyTree(c, tree, cursor.getString(0), file, false, copied, depth + 1);
                    continue;
                }
                long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                if (size > 0 && size > destination.getUsableSpace() - (512L << 20))
                    throw new IOException("Not enough free storage");
                try (InputStream input = c.getContentResolver().openInputStream(
                        DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)));
                     FileOutputStream output = new FileOutputStream(file)) {
                    if (input == null) throw new IOException("Game file is unavailable");
                    byte[] bytes = new byte[1 << 20];
                    int n;
                    while ((n = input.read(bytes)) != -1) {
                        output.write(bytes, 0, n); copied[0] += n;
                    }
                    output.getFD().sync();
                }
                if (size >= 0 && file.length() != size) throw new IOException("Incomplete game file: " + name);
                folderStatus = "Importing… " + (copied[0] >> 20) + " MB copied";
            }
        }
    }

    public static String driverStatus(Context context) {
        if (RaitchiUiDriverImport.isRunning()) return RaitchiUiDriverImport.status();
        GpuDrivers.Driver driver = GpuDrivers.find(context);
        String current = driver == null ? "System GPU driver" :
                driver.name + " " + driver.version + (GpuDrivers.selected(context) ? " · enabled" : " · disabled");
        String result = RaitchiUiDriverImport.status();
        return result.isEmpty() ? current : result + "\n" + current;
    }
    public static boolean isDriverEnabled(Context context) { return GpuDrivers.selected(context); }
    public static void setDriverEnabled(Context context, boolean enabled) throws IOException {
        requireIdle(context);
        if (enabled && GpuDrivers.find(context) == null) throw new IOException("Install a driver ZIP first");
        GpuDrivers.select(context, enabled);
    }
    public static boolean startDriverImport(Context context, Uri document) throws IOException {
        requireIdle(context);
        return RaitchiUiDriverImport.start(context, document);
    }
    public static boolean isDriverImportRunning() { return RaitchiUiDriverImport.isRunning(); }

    /** The Eden driver manager invokes this on its I/O worker, never its UI thread. */
    public static String installDriver(Context owner, Uri source) throws IOException {
        Context app = owner.getApplicationContext();
        requireIdle(app);
        if (!driverInstalling.compareAndSet(false, true)) throw new IOException("A driver is being installed");
        try { return GpuDrivers.install(app, source); }
        finally { driverInstalling.set(false); }
    }

    /** Prepares a scanned, identity-checked package using the setup wizard's private keys. */
    public static boolean prepareScannedGame(Context owner, Uri source, String adapterId) throws IOException {
        Context app = owner.getApplicationContext();
        SharedFiles.checked(adapterId);
        if (gameRunning(app)) {
            String current = SharedFiles.read(new File(app.getFilesDir(), "game-session.json")).optString("title");
            NativePackageFiles.Selection selected = NativePackage.selection(app, adapterId);
            if (adapterId.equals(current) && selected != null && selected.uri.equals(source.toString())) return false;
            throw new IOException("Quit the running game before switching games");
        }
        requireIdle(app);
        RaitchiMetadata.GameInfo info = RaitchiMetadata.inspect(app, source);
        // "generic" tries a game without an adapter; it requires the engine's tryable verdict.
        boolean accepted = TitlePolicy.generic(adapterId) ? info.tryable && !info.compatible : info.compatible;
        if (!accepted || !adapterId.equals(info.adapterId))
            throw new IOException(info.reason.isEmpty() ? "No compatible game adapter" : info.reason);
        NativePackageFiles.Selection selected = NativePackage.selection(app, adapterId);
        if (selected != null && selected.reusable() && selected.uri.equals(source.toString()) && SharedFiles.ready(app, adapterId)) return false;
        NativePackage.start(app, adapterId, source, Uri.fromFile(RaitchiMetadata.keysFile(app)));
        return true;
    }

    public static void cleanupAbandonedImports(Context owner) {
        Context app = owner.getApplicationContext();
        new Thread(() -> RaitchiUiPackageStaging.cleanup(app.getFilesDir(), () -> !isBusy(app)),
                "raitchi-shell-import-cleanup").start();
    }

    private RaitchiShellBridge() {}
}
