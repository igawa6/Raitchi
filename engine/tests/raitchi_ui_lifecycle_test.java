package com.lasrecomp.android;

import android.content.Context;
import android.net.Uri;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Actual launcher state/staging helpers; fake Android driver I/O only. */
final class RaitchiUiLifecycleTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    private static void awaitDone(long revision)throws Exception{
        long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
        while(RaitchiUiDriverImport.isRunning()||RaitchiUiDriverImport.revision()<revision){if(System.nanoTime()>deadline)throw new AssertionError("driver completion timeout");Thread.sleep(1);}
    }
    private static void driverLifecycle()throws Exception{
        Context app=new Context(),first=new Context();first.application=app;
        long before=RaitchiUiDriverImport.revision();check(RaitchiUiDriverImport.start(first,new Uri()));
        check(GpuDrivers.entered.await(3,TimeUnit.SECONDS));check(RaitchiUiDriverImport.isRunning());check(GpuDrivers.received==app);
        Context recreated=new Context();recreated.application=app;
        check(!RaitchiUiDriverImport.start(recreated,new Uri()));
        ExecutorService pool=Executors.newFixedThreadPool(8);List<Future<Boolean>> attempts=new ArrayList<>();
        for(int i=0;i<64;i++)attempts.add(pool.submit(()->RaitchiUiDriverImport.start(recreated,new Uri())));
        for(Future<Boolean> attempt:attempts)check(!attempt.get());pool.shutdown();check(pool.awaitTermination(3,TimeUnit.SECONDS));check(GpuDrivers.calls.get()==1);
        GpuDrivers.release.countDown();awaitDone(before+2);check(!RaitchiUiDriverImport.isRunning());check(RaitchiUiDriverImport.status().equals("Installed test driver"));
        long failedRevision=RaitchiUiDriverImport.revision();check(RaitchiUiDriverImport.begin(()->{throw new IOException("synthetic failure");}));awaitDone(failedRevision+2);
        check(RaitchiUiDriverImport.status().contains("synthetic failure"));long next=RaitchiUiDriverImport.revision();check(RaitchiUiDriverImport.begin(()->"Recovered installation"));awaitDone(next+2);check(RaitchiUiDriverImport.status().equals("Recovered installation"));
    }
    private static File abandoned(File files,String title)throws Exception{
        Process child=new ProcessBuilder(new File(System.getProperty("java.home"),"bin/java").getPath(),"-cp",System.getProperty("java.class.path"),RaitchiUiLifecycleTest.class.getName(),"create",files.getPath(),title).redirectErrorStream(true).start();
        String path=new String(child.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8).trim();check(child.waitFor()==0);File stage=new File(path);check(stage.isDirectory());return stage;
    }
    private static void data(File parent,String relative,String value)throws Exception{Path path=parent.toPath().resolve(relative);Files.createDirectories(path.getParent());Files.writeString(path,value);}
    private static void stagingLifecycle(File files)throws Exception{
        File active=RaitchiUiPackageStaging.begin(files,"las");data(active,"exefs/main","active");check(new File(active,".raitchi-import-owner").isFile());
        check(RaitchiUiPackageStaging.cleanup(files,()->true)==0);check(active.isDirectory());RaitchiUiPackageStaging.discard(files,"las",active);check(!active.exists());
        File dead=abandoned(files,"las");check(RaitchiUiPackageStaging.cleanup(files,()->false)==0&&dead.isDirectory());check(RaitchiUiPackageStaging.cleanup(files,()->true)==1&&!dead.exists());
        File published=RaitchiUiPackageStaging.begin(files,"las");data(published,"exefs/main","published");RaitchiUiPackageStaging.publish(files,"las",published);File game=new File(files,"games/las/extracted");check(!published.exists());check(Files.readString(new File(game,"exefs/main").toPath()).equals("published"));check(!new File(game,".raitchi-import-owner").exists());
        File conflict=RaitchiUiPackageStaging.begin(files,"las");data(conflict,"exefs/main","conflict");boolean rejected=false;try{RaitchiUiPackageStaging.publish(files,"las",conflict);}catch(IOException expected){rejected=true;}check(rejected);check(Files.readString(new File(game,"exefs/main").toPath()).equals("published"));RaitchiUiPackageStaging.discard(files,"las",conflict);check(!conflict.exists());
        RaitchiUiPackageStaging.discard(files,"las",game);check(game.isDirectory());data(files,"sessions/las-1.0.0/save/owner","save remains");
        File parent=new File(files,"games/las"),legacy=new File(parent,".package-"+UUID.randomUUID());check(legacy.mkdir());data(legacy,"romfs/file","legacy");check(RaitchiUiPackageStaging.cleanup(files,()->true)==1&&!legacy.exists());
        File malformedName=new File(parent,".package-not-a-uuid");check(malformedName.mkdir());File nonV4=new File(parent,".package-00000000-0000-1000-8000-000000000000");check(nonV4.mkdir());check(RaitchiUiPackageStaging.cleanup(files,()->true)==0);check(malformedName.isDirectory()&&nonV4.isDirectory());
        for(String key:new String[]{"parent","title","uuid","format"}){
            File invalid=abandoned(files,"las"),manifest=new File(invalid,".raitchi-import-owner");Properties values=new Properties();try(InputStream in=new FileInputStream(manifest)){values.load(in);}values.setProperty(key,"mismatched");try(OutputStream out=new FileOutputStream(manifest)){values.store(out,"");}
            check(RaitchiUiPackageStaging.cleanup(files,()->true)==0);check(invalid.isDirectory());
        }
        File corrupt=abandoned(files,"las");Files.writeString(new File(corrupt,".raitchi-import-owner").toPath(),"broken=\\uZZZZ\n");check(RaitchiUiPackageStaging.cleanup(files,()->true)==0&&corrupt.isDirectory());
        File outside=new File(files.getParentFile(),"outside");check(outside.mkdir());data(outside,"owner","outside remains");File link=new File(parent,".package-"+UUID.randomUUID());Files.createSymbolicLink(link.toPath(),outside.toPath());
        File childLink=new File(parent,".package-"+UUID.randomUUID());check(childLink.mkdir());Files.createSymbolicLink(new File(childLink,"linked").toPath(),outside.toPath());check(RaitchiUiPackageStaging.cleanup(files,()->true)==0);check(Files.isSymbolicLink(link.toPath())&&childLink.isDirectory());check(Files.readString(new File(outside,"owner").toPath()).equals("outside remains"));
        File eevee=new File(files,"games/eevee");check(eevee.mkdir());Files.delete(eevee.toPath());Files.createSymbolicLink(eevee.toPath(),outside.toPath());File outsideStage=new File(outside,".package-"+UUID.randomUUID());check(outsideStage.mkdir());data(outsideStage,"exefs/main","user folder");check(RaitchiUiPackageStaging.cleanup(files,()->true)==0&&outsideStage.isDirectory());
        check(Files.readString(new File(files,"sessions/las-1.0.0/save/owner").toPath()).equals("save remains"));check(Files.readString(new File(game,"exefs/main").toPath()).equals("published"));
    }
    public static void main(String[] args)throws Exception{
        if(args.length>0&&args[0].equals("create")){File stage=RaitchiUiPackageStaging.begin(new File(args[1]),args[2]);data(stage,"exefs/main","abandoned by process exit");System.out.println(stage);return;}
        driverLifecycle();Path test=Files.createTempDirectory("raitchi-lifecycle-");File files=test.resolve("files").toFile();check(files.mkdir());
        try{stagingLifecycle(files);}finally{try(java.util.stream.Stream<Path> paths=Files.walk(test)){for(Path path:paths.sorted(Comparator.reverseOrder()).toList())Files.delete(path);}}
        System.out.println("checks="+checks+" passed; actual lifecycle helpers, synthetic private trees/driver I/O");
    }
}
