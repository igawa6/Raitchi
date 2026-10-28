package com.lasrecomp.android;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Asset-free publication/rollback checks against real app-owned generation code. */
public final class NativePackageFilesTest {
    private static int checks;
    private static void check(boolean b){checks++;if(!b)throw new AssertionError("check "+checks);}
    private interface Attempt{void run()throws Exception;}
    private static void reject(Attempt a)throws Exception {boolean rejected=false;try{a.run();}catch(IOException expected){rejected=true;}check(rejected);}
    private static void write(File f,byte[] data)throws Exception{f.getParentFile().mkdirs();Files.write(f.toPath(),data);}
    private static File fixture(File files,String title)throws Exception {
        File stage=NativePackageFiles.begin(files,title);TitlePolicy policy=TitlePolicy.get(title);int n=policy.modules.length,id=policy.id;
        byte[] manifest=new byte[128+104*n];System.arraycopy(new byte[]{'N','A','T','V','M','0','0','1'},0,manifest,0,8);
        ByteBuffer b=ByteBuffer.wrap(manifest).order(ByteOrder.LITTLE_ENDIAN);b.putInt(8,manifest.length);b.putInt(12,1);b.putInt(16,id);b.putInt(20,n);
        write(new File(stage,"native.manifest"),manifest);write(new File(stage,"prod.keys"),"asset-free-key-placeholder".getBytes());
        for(String name:n==2?new String[]{"main","sdk"}:new String[]{"main","sdk","subsdk0","subsdk1"}) {
            byte[] nso=new byte[256];System.arraycopy(new byte[]{'N','S','O','0'},0,nso,0,4);write(new File(stage,"exefs/"+name),nso);
        }return stage;
    }
    /** Generic manifest: module IDs from the package (1 main, 2 sdk, 3 subsdk0, 4 subsdk1) and its program ID. */
    private static File genericFixture(File files,long program,int... modules)throws Exception {
        File stage=NativePackageFiles.begin(files,"generic");int n=modules.length;
        byte[] manifest=new byte[128+104*n];System.arraycopy(new byte[]{'N','A','T','V','M','0','0','1'},0,manifest,0,8);
        ByteBuffer b=ByteBuffer.wrap(manifest).order(ByteOrder.LITTLE_ENDIAN);b.putInt(8,manifest.length);b.putInt(12,1);b.putInt(16,5);b.putInt(20,n);b.putLong(24,program);
        String[] names={"main","sdk","subsdk0","subsdk1"};
        for(int i=0;i<n;i++){b.putInt(128+104*i,modules[i]);byte[] nso=new byte[256];System.arraycopy(new byte[]{'N','S','O','0'},0,nso,0,4);if(modules[i]>=1&&modules[i]<=4)write(new File(stage,"exefs/"+names[modules[i]-1]),nso);}
        write(new File(stage,"native.manifest"),manifest);write(new File(stage,"prod.keys"),"asset-free-key-placeholder".getBytes());return stage;
    }
    private static void genericTests(File files,String uri)throws Exception {
        long program=0x0100aaaabbbb0000L;String hex="0100aaaabbbb0000";
        check(NativePackageFiles.current(files,"generic")==null);
        File missing=genericFixture(files,program,1,3,2);
        reject(()->NativePackageFiles.publish(files,"generic",missing,uri));check(missing.isDirectory());
        reject(()->NativePackageFiles.publish(files,"generic",missing,uri,"0100aaaabbbb0800","1.0.0"));
        reject(()->NativePackageFiles.publish(files,"generic",missing,uri,hex,"1.0 0"));
        reject(()->NativePackageFiles.publish(files,"generic",missing,uri,"0100AAAABBBB0000","1.0.0"));
        reject(()->NativePackageFiles.publish(files,"generic",missing,uri,"0100aaaabbbb1000","1.0.0"));
        reject(()->NativePackageFiles.publish(files,"las",fixture(files,"las"),uri,hex,"1.0.0"));
        NativePackageFiles.Selection selected=NativePackageFiles.publish(files,"generic",missing,uri,hex,"1.0.0");
        check(!missing.exists()&&selected.programId.equals(hex)&&selected.version.equals("1.0.0"));
        NativePackageFiles.Selection current=NativePackageFiles.current(files,"generic");
        check(current.root.equals(selected.root)&&current.programId.equals(hex)&&current.version.equals("1.0.0")&&current.uri.equals(uri));
        check(current.reusable()&&current.preparationPolicy.equals(TitlePolicy.GENERIC_NATIVE_POLICY));
        // A legacy pointer remains readable for its save/document identity, but
        // cannot reuse an old layout manifest. Migration owns a fresh UUID.
        File legacyPointer=new File(files,"games/generic/native/selection.properties");
        Properties legacy=new Properties();try(InputStream in=new FileInputStream(legacyPointer)){legacy.load(in);}
        legacy.remove("preparationPolicy");try(OutputStream out=new FileOutputStream(legacyPointer)){legacy.store(out,"");}
        byte[] oldManifest=Files.readAllBytes(selected.manifest.toPath()),oldNso=Files.readAllBytes(new File(selected.root,"exefs/main").toPath());
        File save=new File(files,"sessions/"+TitlePolicy.genericNamespace(hex,"1.0.0")+"-nce/save/slot");write(save,"preserve-generic-save".getBytes());
        NativePackageFiles.Selection stale=NativePackageFiles.current(files,"generic");
        check(!stale.reusable()&&stale.root.equals(selected.root)&&stale.programId.equals(hex)&&stale.version.equals("1.0.0")&&stale.uri.equals(uri));
        byte[] pointerSnapshot=Files.readAllBytes(legacyPointer.toPath());
        File failedMigration=genericFixture(files,program,1,3);
        reject(()->NativePackageFiles.publish(files,"generic",failedMigration,uri,hex,"1.0.0"));
        NativePackageFiles.discard(files,"generic",failedMigration);
        check(Arrays.equals(pointerSnapshot,Files.readAllBytes(legacyPointer.toPath()))&&!NativePackageFiles.current(files,"generic").reusable());
        File fresh=genericFixture(files,program,1,3,2);
        NativePackageFiles.Selection migrated=NativePackageFiles.publish(files,"generic",fresh,uri,hex,"1.0.0");
        check(!migrated.root.equals(stale.root)&&migrated.reusable()&&NativePackageFiles.current(files,"generic").reusable());
        check(migrated.programId.equals(stale.programId)&&migrated.version.equals(stale.version)&&migrated.uri.equals(stale.uri));
        check(Arrays.equals(oldManifest,Files.readAllBytes(selected.manifest.toPath()))&&Arrays.equals(oldNso,Files.readAllBytes(new File(selected.root,"exefs/main").toPath())));
        check(Files.readString(save.toPath()).equals("preserve-generic-save"));
        Properties future=new Properties();try(InputStream in=new FileInputStream(legacyPointer)){future.load(in);}
        future.setProperty("preparationPolicy","future-policy-v3");try(OutputStream out=new FileOutputStream(legacyPointer)){future.store(out,"");}
        check(!NativePackageFiles.current(files,"generic").reusable());
        future.setProperty("preparationPolicy","../bad");try(OutputStream out=new FileOutputStream(legacyPointer)){future.store(out,"");}
        reject(()->NativePackageFiles.current(files,"generic"));
        future.setProperty("preparationPolicy",TitlePolicy.GENERIC_NATIVE_POLICY);try(OutputStream out=new FileOutputStream(legacyPointer)){future.store(out,"");}
        File two=genericFixture(files,0x0100cccc00002000L,1,2);
        NativePackageFiles.Selection second=NativePackageFiles.publish(files,"generic",two,uri+"2","0100cccc00002000","2.0");
        check(NativePackageFiles.current(files,"generic").programId.equals("0100cccc00002000")&&selected.root.isDirectory()&&second.root.isDirectory());
        File noSdk=genericFixture(files,program,1,3);reject(()->NativePackageFiles.publish(files,"generic",noSdk,uri,hex,"1.0.0"));NativePackageFiles.discard(files,"generic",noSdk);
        File duplicate=genericFixture(files,program,1,2,2);reject(()->NativePackageFiles.publish(files,"generic",duplicate,uri,hex,"1.0.0"));NativePackageFiles.discard(files,"generic",duplicate);
        File badId=genericFixture(files,program,1,2,5);reject(()->NativePackageFiles.publish(files,"generic",badId,uri,hex,"1.0.0"));NativePackageFiles.discard(files,"generic",badId);
        File five=genericFixture(files,program,1,3,4,2);
        File gone=genericFixture(files,program,1,2,3);Files.delete(new File(gone,"exefs/subsdk0").toPath());reject(()->NativePackageFiles.publish(files,"generic",gone,uri,hex,"1.0.0"));NativePackageFiles.discard(files,"generic",gone);
        File title=genericFixture(files,program,1,2);byte[] m=Files.readAllBytes(new File(title,"native.manifest").toPath());ByteBuffer.wrap(m).order(ByteOrder.LITTLE_ENDIAN).putInt(16,3);Files.write(new File(title,"native.manifest").toPath(),m);
        reject(()->NativePackageFiles.publish(files,"generic",title,uri,hex,"1.0.0"));NativePackageFiles.discard(files,"generic",title);
        check(NativePackageFiles.publish(files,"generic",five,uri,hex,"1.0.0").programId.equals(hex));
        // A selection whose program no longer matches its manifest, or with a damaged version, is rejected.
        File pointer=new File(files,"games/generic/native/selection.properties");String old=Files.readString(pointer.toPath());
        Properties values=new Properties();try(InputStream in=new FileInputStream(pointer)){values.load(in);}
        values.setProperty("program","0100cccc00002000");try(OutputStream out=new FileOutputStream(pointer)){values.store(out,"");}reject(()->NativePackageFiles.current(files,"generic"));
        values.setProperty("program",hex);values.setProperty("version","../x");try(OutputStream out=new FileOutputStream(pointer)){values.store(out,"");}reject(()->NativePackageFiles.current(files,"generic"));
        values.remove("version");try(OutputStream out=new FileOutputStream(pointer)){values.store(out,"");}reject(()->NativePackageFiles.current(files,"generic"));
        Files.writeString(pointer.toPath(),old);check(NativePackageFiles.current(files,"generic").programId.equals(hex));
        check(TitlePolicy.genericVersion("v1.0 (\u00c4)/..").equals("v1.0______..")&&TitlePolicy.genericVersion("").equals("0")&&TitlePolicy.genericVersion(null).equals("0"));
        check(TitlePolicy.genericVersion("12345678901234567890").equals("1234567890123456"));
        check(TitlePolicy.genericNamespace(hex,"1.0.0").equals("generic-0100aaaabbbb0000-1.0.0"));
    }
    public static void main(String[] args)throws Exception {
        File files=Files.createTempDirectory(Path.of(args[0]),"owned-native-test-").toFile();
        String uri="content://test.provider/document/owner-game.nsp";
        File oldSave=new File(files,"sessions/echoes-1.0.0-nce/save/slot/main.bin"),oldCode=new File(files,"code/las/current/liblasgame.so"),oldExtracted=new File(files,"games/las/extracted/romfs/owner");
        for(File f:new File[]{oldSave,oldCode,oldExtracted})write(f,"preserve".getBytes());
        for(String title:new String[]{"las","eevee","echoes","wonder"}) {
            check(NativePackageFiles.current(files,title)==null);File stage=fixture(files,title);
            reject(()->NativePackageFiles.publish(files,title,stage,uri+"\nINJECT=1"));check(stage.isDirectory());
            NativePackageFiles.Selection selected=NativePackageFiles.publish(files,title,stage,uri);
            check(!stage.exists());check(selected.root.isDirectory());check(selected.keys.isFile());check(selected.manifest.isFile());
            check(NativePackageFiles.current(files,title).root.equals(selected.root));check(selected.uri.equals(uri));
            check(selected.reusable()&&selected.preparationPolicy.isEmpty());
            Properties pointerValues=new Properties();try(InputStream in=new FileInputStream(new File(files,"games/"+title+"/native/selection.properties"))){pointerValues.load(in);}
            check(pointerValues.size()==4&&!pointerValues.containsKey("preparationPolicy"));
            File second=fixture(files,title);NativePackageFiles.Selection newer=NativePackageFiles.publish(files,title,second,uri+"2");
            check(!newer.root.equals(selected.root));check(selected.root.isDirectory());check(NativePackageFiles.current(files,title).root.equals(newer.root));
        }
        for(File f:new File[]{oldSave,oldCode,oldExtracted})check(Files.readString(f.toPath()).equals("preserve"));
        File invalid=fixture(files,"echoes");Files.delete(new File(invalid,"exefs/subsdk1").toPath());
        reject(()->NativePackageFiles.publish(files,"echoes",invalid,uri));NativePackageFiles.discard(files,"echoes",invalid);check(!invalid.exists());
        File wonder=fixture(files,"wonder");Files.delete(new File(wonder,"exefs/sdk").toPath());reject(()->NativePackageFiles.publish(files,"wonder",wonder,uri));NativePackageFiles.discard(files,"wonder",wonder);
        File wrong=fixture(files,"las");reject(()->NativePackageFiles.publish(files,"eevee",wrong,uri));check(wrong.exists());NativePackageFiles.discard(files,"las",wrong);
        File romfs=fixture(files,"echoes");new File(romfs,"romfs").mkdir();reject(()->NativePackageFiles.publish(files,"echoes",romfs,uri));NativePackageFiles.discard(files,"echoes",romfs);
        File linked=fixture(files,"las");Path sdk=new File(linked,"exefs/sdk").toPath();Files.delete(sdk);Files.createSymbolicLink(sdk,oldCode.toPath());
        reject(()->NativePackageFiles.publish(files,"las",linked,uri));NativePackageFiles.discard(files,"las",linked);check(oldCode.isFile());
        File unrelated=new File(files,"games/las/extracted");NativePackageFiles.discard(files,"las",unrelated);check(oldExtracted.isFile());
        File active=NativePackageFiles.current(files,"echoes").root;NativePackageFiles.discard(files,"echoes",active);check(active.isDirectory());
        File pointer=new File(files,"games/echoes/native/selection.properties");String old=Files.readString(pointer.toPath());
        Properties values=new Properties();try(InputStream in=new FileInputStream(pointer)){values.load(in);}values.setProperty("generation","../../extracted");try(OutputStream out=new FileOutputStream(pointer)){values.store(out,"");}
        reject(()->NativePackageFiles.current(files,"echoes"));Files.writeString(pointer.toPath(),old);
        check(NativePackageFiles.current(files,"echoes").root.equals(active));
        genericTests(files,uri);
        File alias=new File(files,"games/unrelated");alias.mkdirs();reject(()->NativePackageFiles.begin(files,"unrelated"));
        System.out.println("Native package generation checks="+checks+"; no real game/JNI/device execution");
    }
}
