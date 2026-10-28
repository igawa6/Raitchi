package com.lasrecomp.android;

import android.content.Context;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.database.Cursor;

import java.io.FileNotFoundException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Host regression for the production shell bridge with a minimal SAF/filesystem adapter. */
public final class RaitchiShellBridgeTest {
    private static int checks;
    private static File root;

    private static final Map<String,MemoryPreferences> preferences=new HashMap<>();
    private static class MemoryPreferences implements SharedPreferences {
        final Map<String,String> values=new HashMap<>();
        public synchronized String getString(String key,String fallback){return values.getOrDefault(key,fallback);}
        public synchronized boolean contains(String key){return values.containsKey(key);}
        public Editor edit(){return new Edit(this);}
        static class Edit implements Editor {
            final MemoryPreferences parent;final Map<String,String> put=new HashMap<>();final List<String> remove=new ArrayList<>();
            Edit(MemoryPreferences parent){this.parent=parent;}
            public Editor putString(String k,String v){put.put(k,v);return this;}
            public Editor remove(String k){remove.add(k);return this;}
            public boolean commit(){synchronized(parent){for(String k:remove)parent.values.remove(k);parent.values.putAll(put);}return true;}
        }
    }
    private static final class TestContext extends Context {
        final File files;final Context app;ContentResolver resolver;int serviceStarts;
        TestContext(File files,Context app){this.files=files;this.app=app==null?this:app;this.resolver=new FileResolver();}
        public Context getApplicationContext(){return app;}
        public File getFilesDir(){return files;}
        public ContentResolver getContentResolver(){return resolver;}
        public SharedPreferences getSharedPreferences(String name,int mode){
            String key;try{key=files.getCanonicalPath()+":"+name;}catch(IOException e){throw new RuntimeException(e);}
            synchronized(preferences){return preferences.computeIfAbsent(key,k->new MemoryPreferences());}
        }
        public Intent startForegroundService(Intent intent){serviceStarts++;return intent;}
    }
    private static class FileResolver extends ContentResolver {
        int queries;
        public Cursor query(Uri uri,String[] columns,String selection,String[] args,String order){
            queries++;if("malformed".equals(uri.getId()))return null;
            File dir=new File(uri.getId());File[] entries=dir.listFiles();if(entries==null)return null;
            Arrays.sort(entries,(a,b)->a.getName().compareTo(b.getName()));return new FileCursor(entries);
        }
        public InputStream openInputStream(Uri uri)throws FileNotFoundException{return new java.io.FileInputStream(new File(uri.getId()));}
    }
    private static final class RejectingResolver extends ContentResolver {
        public Cursor query(Uri uri,String[] columns,String selection,String[] args,String order){throw new AssertionError("Activity resolver used by worker");}
        public InputStream openInputStream(Uri uri)throws FileNotFoundException{throw new AssertionError("Activity resolver used by worker");}
    }
    private static final class FileCursor implements Cursor {
        final File[] files;int at=-1;FileCursor(File[] files){this.files=files;}
        public boolean moveToNext(){return ++at<files.length;}
        public String getString(int column){File f=files[at];return column==0?f.getAbsolutePath():column==1?f.getName():f.isDirectory()?"vnd.android.document/directory":"application/octet-stream";}
        public boolean isNull(int column){return false;}
        public long getLong(int column){return files[at].length();}
        public void close(){}
    }

    private static void check(boolean value,String message) {
        checks++; if(!value)throw new AssertionError(message);
    }
    private static void expectIo(Throwing action,String message) throws Exception {
        checks++;
        try { action.run(); } catch(IOException expected) { return; }
        throw new AssertionError(message);
    }
    private static void expectIllegal(Throwing action,String message) throws Exception {
        checks++;
        try { action.run(); } catch(IllegalArgumentException expected) { return; }
        throw new AssertionError(message);
    }
    private static boolean hasOption(SettingsStore.Option[] options,String key) {
        for(SettingsStore.Option option:options)if(option.key.equals(key))return true;
        return false;
    }
    private interface Throwing { void run() throws Exception; }

    private static void write(File file,byte[] bytes) throws Exception {
        File parent=file.getParentFile(); if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("mkdir "+parent);
        try(FileOutputStream out=new FileOutputStream(file)){out.write(bytes);out.getFD().sync();}
    }
    private static byte[] read(File file) throws Exception { return Files.readAllBytes(file.toPath()); }
    private static void waitForImport() throws Exception {
        long until=System.currentTimeMillis()+10000;
        while(RaitchiShellBridge.isFolderImportRunning()&&System.currentTimeMillis()<until)Thread.sleep(10);
        check(!RaitchiShellBridge.isFolderImportRunning(),"folder worker finished");
    }
    private static File sourceTree(String name,boolean complete) throws Exception {
        File tree=new File(root,name);
        write(new File(tree,"exefs/main"),new byte[]{1,2,3,4});
        write(new File(tree,"exefs/sdk"),new byte[]{5,6,7});
        if(complete)write(new File(tree,"romfs/region_common/message"),new byte[]{8,9,10,11});
        return tree;
    }

    public static void main(String[] args) throws Exception {
        root=new File(System.getProperty("raitchi.test.root"));
        delete(root); if(!root.mkdirs())throw new IOException("test root mkdir");
        TestContext app=new TestContext(new File(root,"app"),null);
        TestContext owner=new TestContext(app.getFilesDir(),app);
        owner.resolver=new RejectingResolver();

        // Public API gates reject unsupported IDs, malformed folder URIs, and non-SAF game/key URIs.
        expectIllegal(()->RaitchiShellBridge.importExtractedFolder(owner,"unknown",Uri.tree("unused")),"unsupported title rejected");
        expectIo(()->RaitchiShellBridge.importExtractedFolder(owner,"las",null),"null folder URI rejected synchronously");
        expectIo(()->RaitchiShellBridge.importExtractedFolder(owner,"las",Uri.content("document",false)),"document URI rejected as tree");
        expectIo(()->RaitchiShellBridge.beginPackageImport(owner,"las",Uri.file("/tmp/game.nsp"),Uri.file("/tmp/prod.keys")),"file URIs rejected for persistent package access");

        // A running game blocks imports and cross-title starts. resumeSession cannot start an idle process.
        SharedLauncherActivity.gamePid=123;
        expectIo(()->RaitchiShellBridge.importExtractedFolder(owner,"las",Uri.tree("unused")),"running game blocks folder import");
        SharedFiles.atomic(new File(app.getFilesDir(),"game-session.json"),"{\"title\":\"las\"}");
        expectIo(()->RaitchiShellBridge.prepareSession(owner,"eevee"),"running game blocks title switch");
        Intent resume=RaitchiShellBridge.prepareSession(owner,"las");
        check(resume.getClassName().equals("com.lasrecomp.android.LasActivity"),"same-title resume targets SDL activity");
        check((resume.getFlags()&Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)!=0,"same-title resume reorders existing activity");
        SharedLauncherActivity.gamePid=0;
        expectIo(()->RaitchiShellBridge.resumeSession(owner),"resume without a running game rejected");

        // Scanned package launch is gated by metadata compatibility and adapter identity.
        Uri scanned=Uri.content("owned-package",false);
        int packageStarts=NativePackage.starts;
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("las",false,"Unsupported main build prefix");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,scanned,"las"),"unsupported scanned build rejected");
        check(NativePackage.starts==packageStarts,"unsupported scan does not start preparation");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("echoes",true,"");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,scanned,"las"),"adapter mismatch rejected");
        check(NativePackage.starts==packageStarts,"adapter mismatch does not start preparation");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("las",true,"");
        check(RaitchiShellBridge.prepareScannedGame(owner,scanned,"las"),"compatible scan starts preparation");
        check(NativePackage.starts==packageStarts+1&&"las".equals(NativePackage.lastTitle),"compatible adapter starts NativePackage");
        check(NativePackage.lastKeys.getScheme().equals("file")&&NativePackage.lastKeys.getId().equals(RaitchiMetadata.keysFile(app).getAbsolutePath()),"preparation uses the app-private imported keys file");
        NativePackage.SELECTIONS.put("las",new NativePackageFiles.Selection(new File(app.getFilesDir(),"native/las"),"las",scanned.toString()));
        SharedFiles.READY.put("las",true);
        check(!RaitchiShellBridge.prepareScannedGame(owner,scanned,"las"),"same prepared URI is reused");
        check(NativePackage.starts==packageStarts+1,"reuse does not start another worker");
        SharedLauncherActivity.gamePid=123;
        SharedFiles.atomic(new File(app.getFilesDir(),"game-session.json"),"{\"title\":\"las\"}");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,Uri.content("other-package",false),"eevee"),"running game blocks cross-title scanned package");
        SharedLauncherActivity.gamePid=0;
        SharedFiles.READY.clear();NativePackage.SELECTIONS.clear();

        // Generic startup: only an engine "tryable" verdict for adapterId "generic" starts preparation.
        check("generic".equals(RaitchiShellBridge.GENERIC_ADAPTER),"generic adapter key is stable");
        Uri unknown=Uri.content("unknown-package",false);
        packageStarts=NativePackage.starts;
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("",false,false,"No Raitchi adapter matches this program ID; generic startup cannot try it: This is a 32-bit program");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"non-tryable unknown package rejected");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("generic",false,false,"not tryable");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"generic without tryable verdict rejected");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("generic",true,true,"inconsistent");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"compatible package never prepares as generic");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("las",true,false,"");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"supported adapter cannot be tried as generic");
        RaitchiMetadata.next=new RaitchiMetadata.GameInfo("generic",false,true,"No Raitchi adapter for this game yet; it can be tried with generic startup and may close unexpectedly");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"las"),"tryable package cannot claim a specific adapter");
        expectIllegal(()->RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic2"),"unknown adapter key rejected");
        check(NativePackage.starts==packageStarts,"rejected generic requests start nothing");
        check(RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"tryable package starts generic preparation");
        check(NativePackage.starts==packageStarts+1&&"generic".equals(NativePackage.lastTitle)&&NativePackage.lastGame==unknown,"generic uses the same NativePackage path");
        NativePackage.SELECTIONS.put("generic",new NativePackageFiles.Selection(new File(app.getFilesDir(),"native/generic"),"generic",unknown.toString(),"0100aaaabbbb0000","1.2.0"));
        SharedFiles.READY.put("generic",true);
        check(RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"legacy generic policy starts fresh preparation even if URI/readiness match");
        check(NativePackage.starts==packageStarts+2,"legacy migration starts one worker");
        NativePackage.SELECTIONS.put("generic",new NativePackageFiles.Selection(new File(app.getFilesDir(),"native/generic"),"generic",unknown.toString(),"0100aaaabbbb0000","1.2.0","future-policy-v3"));
        check(RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"unknown preparation policy cannot reuse generic cache");
        NativePackage.SELECTIONS.put("generic",new NativePackageFiles.Selection(new File(app.getFilesDir(),"native/generic"),"generic",unknown.toString(),"0100aaaabbbb0000","1.2.0",TitlePolicy.GENERIC_NATIVE_POLICY));
        check(!RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"same prepared generic URI and current policy are reused");
        check(RaitchiShellBridge.prepareScannedGame(owner,Uri.content("another-unknown",false),"generic"),"another tryable game replaces the generic preparation");
        check(NativePackage.starts==packageStarts+4,"migration, unknown-policy and replacement each start one worker");
        SharedLauncherActivity.gamePid=123;
        SharedFiles.atomic(new File(app.getFilesDir(),"game-session.json"),"{\"title\":\"generic\"}");
        check(!RaitchiShellBridge.prepareScannedGame(owner,unknown,"generic"),"running generic game with the same URI is not prepared again");
        expectIo(()->RaitchiShellBridge.prepareScannedGame(owner,Uri.content("third-unknown",false),"generic"),"running generic game blocks another generic game");
        Intent genericResume=RaitchiShellBridge.prepareSession(owner,"generic");
        check((genericResume.getFlags()&Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)!=0,"generic session resumes like supported titles");
        check(RaitchiShellBridge.resumeSession(owner).getClassName().equals("com.lasrecomp.android.LasActivity"),"generic running session is resumable");
        SharedLauncherActivity.gamePid=0;
        SharedFiles.READY.clear();NativePackage.SELECTIONS.clear();
        check(RaitchiShellBridge.isAdapterExperimental("generic"),"generic startup is experimental");
        check(!RaitchiShellBridge.supportsRenderScaling("generic"),"generic startup has no qualified render scaling");
        SettingsStore.Option[] genericOptions=RaitchiShellBridge.settingOptions("generic");
        check(!hasOption(genericOptions,"cpu_backend")&&!hasOption(genericOptions,"frame_rate")&&!hasOption(genericOptions,"aspect")&&
                hasOption(genericOptions,"render_scale")&&hasOption(genericOptions,"language"),"generic settings exclude LAS-only options");
        RaitchiShellBridge.setSetting(owner,"generic","volume","0.25");
        check("0.25".equals(RaitchiShellBridge.getSetting(owner,"generic","volume"))&&"continuous-nce".equals(RaitchiShellBridge.getSetting(owner,"generic","cpu_backend")),"generic settings persist under the generic key and force NCE");
        expectIllegal(()->RaitchiShellBridge.setSetting(owner,"generic","cpu_backend","aot"),"generic cannot select AOT");
        for(RaitchiShellBridge.Title listed:RaitchiShellBridge.titles(app))check(!"generic".equals(listed.id),"generic is not listed as a supported title");

        // Engine-facing settings expose the union of supported options but no CPU backend;
        // legacy LAS-only options are filtered for Eevee and Echoes.
        check(RaitchiShellBridge.isAdapterExperimental("wonder"),"experimental adapter capability comes from engine policy");
        check(RaitchiShellBridge.supportsRenderScaling(null)&&RaitchiShellBridge.supportsRenderScaling("las"),
                "global resolution choice and qualified LAS scaling remain available");
        check(!RaitchiShellBridge.supportsRenderScaling("eevee")&&!RaitchiShellBridge.supportsRenderScaling("echoes")&&
                !RaitchiShellBridge.supportsRenderScaling("wonder"),"unqualified adapters use the native-size settings notice");
        for(String adapter:new String[]{"las","eevee","echoes","unknown","",null})
            check(!RaitchiShellBridge.isAdapterExperimental(adapter),"stable or unknown adapter is not experimental");
        SettingsStore.Option[] globalOptions=RaitchiShellBridge.settingOptions(null);
        SettingsStore.Option[] lasOptions=RaitchiShellBridge.settingOptions("las");
        SettingsStore.Option[] eeveeOptions=RaitchiShellBridge.settingOptions("eevee");
        SettingsStore.Option[] echoesOptions=RaitchiShellBridge.settingOptions("echoes");
        check(!hasOption(globalOptions,"cpu_backend")&&!hasOption(lasOptions,"cpu_backend")&&
                !hasOption(eeveeOptions,"cpu_backend")&&!hasOption(echoesOptions,"cpu_backend"),"NCE engine UI never exposes CPU backend selector");
        check(hasOption(globalOptions,"frame_rate")&&hasOption(lasOptions,"frame_rate"),"global settings retain LAS legacy options");
        check(!hasOption(eeveeOptions,"frame_rate")&&!hasOption(echoesOptions,"frame_rate"),"engine filters LAS-only settings for other adapters");

        // Driver installation is busy-gated and the direct installer flag clears on failure.
        RaitchiUiDriverImport.running=true;
        int driverCalls=GpuDrivers.installs;
        expectIo(()->RaitchiShellBridge.installDriver(owner,Uri.file("driver.zip")),"active driver worker blocks duplicate install");
        check(GpuDrivers.installs==driverCalls,"busy driver guard prevents installer call");
        RaitchiUiDriverImport.running=false;GpuDrivers.failInstall=true;
        expectIo(()->RaitchiShellBridge.installDriver(owner,Uri.file("broken-driver.zip")),"installer failure propagates to caller");
        check(!RaitchiShellBridge.isBusy(owner),"failed installer releases busy state");
        GpuDrivers.failInstall=false;
        check(RaitchiShellBridge.installDriver(owner,Uri.file("driver.zip")).equals("Installed test driver"),"driver install can run after failed attempt");

        // Successful SAF import uses application Context on the worker and copies real source bytes.
        File good=sourceTree("saf-good",true);
        Uri goodUri=Uri.tree(good.getAbsolutePath());
        RaitchiShellBridge.importExtractedFolder(owner,"las",goodUri); waitForImport();
        File imported=SharedFiles.game(app,"las");
        check(SharedFiles.extractedImported(app,"las"),"valid folder published as imported");
        check(Arrays.equals(read(new File(imported,"exefs/main")),new byte[]{1,2,3,4}),"ExeFS bytes copied from SAF source");
        check(Arrays.equals(read(new File(imported,"romfs/region_common/message")),new byte[]{8,9,10,11}),"RomFS bytes copied from SAF source");
        check(((FileResolver)app.resolver).queries>0&&!(owner.resolver instanceof FileResolver),"worker uses application Context resolver");

        // A second complete import cannot replace or mutate the user's complete copy.
        byte[] before=read(new File(imported,"exefs/main"));
        expectIo(()->RaitchiShellBridge.importExtractedFolder(owner,"las",Uri.tree(good.getAbsolutePath())),"complete import protected");
        check(Arrays.equals(read(new File(imported,"exefs/main")),before),"protected import bytes unchanged");

        // Bad SAF contents fail in the owned stage; no partial extracted target or orphan stage is published.
        TestContext malformedContext=new TestContext(new File(root,"malformed-app"),null);
        File malformed=sourceTree("saf-malformed",false);
        RaitchiShellBridge.importExtractedFolder(malformedContext,"las",Uri.tree(malformed.getAbsolutePath())); waitForImport();
        File malformedParent=new File(malformedContext.getFilesDir(),"games/las");
        check(!new File(malformedParent,"extracted").exists(),"malformed tree not published");
        check(noChildrenWithPrefix(malformedParent,".package-"),"failed stage cleaned");
        check(RaitchiShellBridge.folderImportStatus().startsWith("Import stopped:"),"malformed failure reported");

        // An incomplete legacy target is preserved verbatim, then a fresh valid tree can be published.
        TestContext recoveryContext=new TestContext(new File(root,"recovery-app"),null);
        File old=SharedFiles.game(recoveryContext,"las");byte[] partial={42,43,44};write(new File(old,"exefs/main"),partial);
        File recoverySource=sourceTree("saf-recovery",true);
        RaitchiShellBridge.importExtractedFolder(recoveryContext,"las",Uri.tree(recoverySource.getAbsolutePath())); waitForImport();
        check(SharedFiles.extractedImported(recoveryContext,"las"),"replacement complete tree published");
        check(Arrays.equals(read(new File(SharedFiles.game(recoveryContext,"las"),"exefs/main")),new byte[]{1,2,3,4}),"new import bytes published");
        File[] preserved=new File(recoveryContext.getFilesDir(),"games/las").listFiles((dir,name)->name.startsWith(".incomplete-"));
        check(preserved!=null&&preserved.length==1,"incomplete legacy target retained");
        check(Arrays.equals(read(new File(preserved[0],"exefs/main")),partial),"preserved partial bytes unchanged");

        // The bridge settings facade delegates validation and persistence to the real SettingsStore.
        RaitchiShellBridge.setSetting(owner,"las","cpu_backend","aot");
        check("aot".equals(RaitchiShellBridge.getSetting(app,"las","cpu_backend")),"settings persist through app context");
        expectIllegal(()->RaitchiShellBridge.setSetting(owner,"las","cpu_backend","invalid"),"invalid setting rejected");

        delete(root);
        System.out.println(checks+" shell bridge assertions passed (production bridge/staging/settings; SAF and Android APIs stubbed)");
    }

    private static boolean noChildrenWithPrefix(File dir,String prefix) {
        File[] rows=dir.listFiles((parent,name)->name.startsWith(prefix));return rows!=null&&rows.length==0;
    }
    private static void delete(File file) throws IOException {
        if(!file.exists())return;File[] children=file.listFiles();if(children!=null)for(File child:children)delete(child);
        if(!file.delete())throw new IOException("delete "+file);
    }
}
