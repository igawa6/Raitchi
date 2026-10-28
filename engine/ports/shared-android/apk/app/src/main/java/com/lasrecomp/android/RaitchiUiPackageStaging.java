package com.lasrecomp.android;

import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Owns only UUID temporary children of this app's fixed games/<title> parent. */
final class RaitchiUiPackageStaging {
    private static final String OWNER=".raitchi-import-owner";
    private static final String FORMAT="raitchi-package-import-v1";
    private static final Set<String> active=new HashSet<>();
    private static final Object lock=new Object();
    private static File parent(File files,String title,boolean create)throws IOException{
        if(!title.equals("las")&&!title.equals("eevee"))throw new IOException("Unsupported title");
        File root=files.getCanonicalFile(),games=new File(root,"games"),parent=new File(games,title);
        if(Files.isSymbolicLink(games.toPath())||Files.isSymbolicLink(parent.toPath())||!games.getCanonicalFile().equals(games)||!parent.getCanonicalFile().equals(parent))throw new IOException("Unsafe game import parent");
        if(create&&!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Cannot create game import parent");
        return parent;
    }
    private static String uuid(File stage)throws IOException{
        String name=stage.getName();if(!name.startsWith(".package-"))throw new IOException("Not an app import directory");
        String text=name.substring(9);try{UUID id=UUID.fromString(text);if(id.version()!=4||id.variant()!=2||!id.toString().equals(text))throw new IllegalArgumentException();}catch(IllegalArgumentException error){throw new IOException("Not an app import UUID");}return text;
    }
    private static void validateLocation(File parent,File stage)throws IOException{
        uuid(stage);if(!stage.getAbsoluteFile().getParentFile().equals(parent)||Files.isSymbolicLink(stage.toPath())||!stage.getCanonicalFile().equals(stage.getAbsoluteFile())||!stage.isDirectory())throw new IOException("Unsafe temporary import path");
    }
    private static void manifest(File parent,String title,File stage)throws IOException{
        Properties values=new Properties();values.setProperty("format",FORMAT);values.setProperty("title",title);values.setProperty("uuid",uuid(stage));values.setProperty("parent",parent.getCanonicalPath());
        File owner=new File(stage,OWNER);if(owner.exists())throw new IOException("Import ownership already exists");
        try(FileOutputStream out=new FileOutputStream(owner)){values.store(out,"Raitchi owned temporary extraction");out.getFD().sync();}
    }
    private static boolean owned(File parent,String title,File stage)throws IOException{
        validateLocation(parent,stage);File owner=new File(stage,OWNER);if(Files.isSymbolicLink(owner.toPath())||!owner.isFile())return false;
        Properties values=new Properties();try(InputStream in=new FileInputStream(owner)){values.load(in);}catch(IllegalArgumentException error){throw new IOException("Invalid import ownership",error);}
        return values.size()==4&&FORMAT.equals(values.getProperty("format"))&&title.equals(values.getProperty("title"))&&uuid(stage).equals(values.getProperty("uuid"))&&parent.getCanonicalPath().equals(values.getProperty("parent"));
    }
    private static void noLinks(File file)throws IOException{
        if(Files.isSymbolicLink(file.toPath()))throw new IOException("Temporary import contains a symbolic link");
        if(file.isDirectory()){File[] children=file.listFiles();if(children==null)throw new IOException("Cannot inspect temporary import");for(File child:children)noLinks(child);}
    }
    private static void remove(File file)throws IOException{
        if(Files.isSymbolicLink(file.toPath()))throw new IOException("Cannot remove a symbolic import link");
        if(file.isDirectory()){File[] children=file.listFiles();if(children==null)throw new IOException("Cannot read temporary import");for(File child:children)remove(child);}
        if(!file.delete())throw new IOException("Cannot remove temporary import");
    }
    private static void removeOwned(File stage)throws IOException{
        noLinks(stage);File[] children=stage.listFiles();if(children==null)throw new IOException("Cannot read temporary import");
        // Keep ownership until every game-data child was removed, for retry after interruption.
        for(File child:children)if(!child.getName().equals(OWNER))remove(child);
        if(!new File(stage,OWNER).delete()||!stage.delete())throw new IOException("Cannot finish temporary import cleanup");
    }
    static File begin(File files,String title)throws IOException{
        synchronized(lock){File parent=parent(files,title,true),stage=new File(parent,".package-"+UUID.randomUUID());if(!stage.mkdir())throw new IOException("Cannot start package import");try{manifest(parent,title,stage);}catch(IOException error){stage.delete();throw error;}active.add(stage.getCanonicalPath());return stage;}
    }
    static void publish(File files,String title,File stage)throws IOException{
        synchronized(lock){File parent=parent(files,title,false);if(!owned(parent,title,stage)||!active.contains(stage.getCanonicalPath()))throw new IOException("Cannot publish unowned import");
            File target=new File(parent,"extracted");if(Files.isSymbolicLink(target.toPath()))throw new IOException("Unsafe game destination");
            if(target.exists()&&!target.delete())throw new IOException("Existing game folder changed; import was not published");
            File owner=new File(stage,OWNER);if(!owner.delete())throw new IOException("Cannot finish import ownership");
            if(!stage.renameTo(target)){manifest(parent,title,stage);throw new IOException("Cannot publish imported game files");}
            active.remove(stage.getAbsolutePath());
        }
    }
    static void discard(File files,String title,File stage){
        if(stage==null)return;synchronized(lock){try{File parent=parent(files,title,false);if(owned(parent,title,stage))removeOwned(stage);}catch(IOException ignored){}finally{active.remove(stage.getAbsolutePath());}}
    }
    static int cleanup(File files,BooleanSupplier idle){
        synchronized(lock){int removed=0;for(String title:new String[]{"las","eevee"}){
            if(!idle.getAsBoolean())break;File parent;try{parent=parent(files,title,false);}catch(IOException error){continue;}
            File[] entries=parent.listFiles();if(entries==null)continue;
            for(File stage:entries){if(!idle.getAsBoolean())return removed;try{
                validateLocation(parent,stage);if(active.contains(stage.getCanonicalPath()))continue;File owner=new File(stage,OWNER);
                if(!owner.exists()){
                    // Legacy versions created precisely UUID-v4 children at this private parent.
                    // Validate the complete location/name and reject all links before claiming it.
                    noLinks(stage);manifest(parent,title,stage);
                }
                if(owned(parent,title,stage)){removeOwned(stage);removed++;}
            }catch(IOException ignored){/* Unknown/user folders and malformed ownership stay untouched. */}}
        }return removed;}
    }
    private RaitchiUiPackageStaging(){}
}
