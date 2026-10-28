package com.lasrecomp.android;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.*;
import java.util.*;

/** Immutable, app-owned native generations. Never replaces extracted files, code or saves. */
final class NativePackageFiles {
    static final class Selection {
        final File root,keys,manifest; final String uri,title;
        /** Generic games only: 16-hex program ID and version token; empty otherwise. */
        final String programId,version,preparationPolicy;
        Selection(File root,String title,String uri){this(root,title,uri,"","");}
        Selection(File root,String title,String uri,String programId,String version){this(root,title,uri,programId,version,"");}
        Selection(File root,String title,String uri,String programId,String version,String preparationPolicy){this.root=root;this.title=title;this.uri=uri;this.programId=programId;this.version=version;this.preparationPolicy=preparationPolicy;keys=new File(root,"prod.keys");manifest=new File(root,"native.manifest");}
        boolean reusable(){return !TitlePolicy.generic(title)||TitlePolicy.GENERIC_NATIVE_POLICY.equals(preparationPolicy);}
    }
    private static void title(String t)throws IOException {try{TitlePolicy.get(t);}catch(IllegalArgumentException e){throw new IOException("Unsupported title",e);}}
    private static File parent(File files,String t)throws IOException {
        title(t);File base=files.getCanonicalFile(),p=new File(base,"games/"+t+"/native");
        for(File f=p;!f.equals(base);f=f.getParentFile())if(Files.isSymbolicLink(f.toPath()))throw new IOException("Unsafe native game path");
        if(!p.getCanonicalFile().equals(p))throw new IOException("Unsafe native game path");return p;
    }
    private static String uuid(String s)throws IOException {try{UUID u=UUID.fromString(s);if(u.version()!=4||u.variant()!=2||!u.toString().equals(s))throw new IllegalArgumentException();return s;}catch(Exception e){throw new IOException("Invalid native generation");}}
    private static void uri(String s)throws IOException {if(!s.startsWith("content://")||s.indexOf('\n')>=0||s.indexOf('\r')>=0||s.indexOf('\0')>=0)throw new IOException("Invalid game document");}
    static File begin(File files,String t)throws IOException {
        File p=parent(files,t);if(!p.isDirectory()&&!p.mkdirs())throw new IOException("Cannot create native game folder");
        File stage=new File(p,".incoming-"+UUID.randomUUID());if(!stage.mkdir())throw new IOException("Cannot start native preparation");return stage;
    }
    private static void validate(File root,String t,String program)throws IOException {
        title(t);if(!root.isDirectory()||Files.isSymbolicLink(root.toPath()))throw new IOException("Native game folder is unavailable");
        File manifest=new File(root,"native.manifest");
        if(Files.isSymbolicLink(manifest.toPath())||!manifest.isFile()||manifest.length()>1024)throw new IOException("Native manifest is unavailable");
        byte[] bytes=Files.readAllBytes(manifest.toPath());TitlePolicy policy=TitlePolicy.get(t);String[] modules=policy.modules;
        if(TitlePolicy.generic(t))modules=genericModules(bytes,policy.id,program);
        else {
        int count=policy.modules.length,id=policy.id;
        if(bytes.length!=128+104*count||!Arrays.equals(Arrays.copyOf(bytes,8),new byte[]{'N','A','T','V','M','0','0','1'}))throw new IOException("Invalid native manifest");
        ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        if(b.getInt(8)!=bytes.length||b.getInt(12)!=1||b.getInt(16)!=id||b.getInt(20)!=count)throw new IOException("Native game identity differs");
        }
        File exefs=new File(root,"exefs");if(Files.isSymbolicLink(exefs.toPath()))throw new IOException("Unsafe ExeFS path");
        for(String module:modules) {
            File f=new File(exefs,module);if(Files.isSymbolicLink(f.toPath())||!f.isFile()||f.length()<256)throw new IOException("Missing original module: "+module);
            try(DataInputStream in=new DataInputStream(new FileInputStream(f))){if(in.readInt()!=0x4e534f30)throw new IOException("Invalid original module: "+module);}
        }
        File keys=new File(root,"prod.keys");if(Files.isSymbolicLink(keys.toPath())||!keys.isFile()||keys.length()==0||keys.length()>(8L<<20))throw new IOException("Private keys file is unavailable");
        if(new File(root,"romfs").exists()||new File(root,"romfs.bin").exists())throw new IOException("Unexpected extracted RomFS in native preparation");
    }
    /** Generic manifests name their own module set; identity is bound to the selection's program ID. */
    private static String[] genericModules(byte[] bytes,int id,String program)throws IOException {
        if(bytes.length<128+104*2||bytes.length>128+104*4||(bytes.length-128)%104!=0||!Arrays.equals(Arrays.copyOf(bytes,8),new byte[]{'N','A','T','V','M','0','0','1'}))throw new IOException("Invalid native manifest");
        ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);int count=b.getInt(20);
        if(bytes.length!=128+104*count||b.getInt(8)!=bytes.length||b.getInt(12)!=1||b.getInt(16)!=id||!TitlePolicy.genericProgram(program)||
                !String.format(Locale.ROOT,"%016x",b.getLong(24)).equals(program))throw new IOException("Native game identity differs");
        String[] names={"main","sdk","subsdk0","subsdk1"},result=new String[count];Set<Integer> seen=new HashSet<>();
        for(int i=0;i<count;i++){int module=b.getInt(128+104*i);if(module<1||module>4||!seen.add(module))throw new IOException("Invalid native manifest");result[i]=names[module-1];}
        if(!seen.contains(1)||!seen.contains(2))throw new IOException("Invalid native manifest");
        return result;
    }
    static Selection current(File files,String t)throws IOException {
        File p=parent(files,t),pointer=new File(p,"selection.properties");if(!pointer.exists())return null;
        if(Files.isSymbolicLink(pointer.toPath())||pointer.length()>16384)throw new IOException("Invalid native selection");
        Properties v=new Properties();try(InputStream in=new FileInputStream(pointer)){v.load(in);}
        boolean generic=TitlePolicy.generic(t);
        int expected=generic?(v.containsKey("preparationPolicy")?7:6):4;
        if(v.size()!=expected||!"raitchi-native-package-v1".equals(v.getProperty("format"))||!t.equals(v.getProperty("title")))throw new IOException("Invalid native selection");
        String program=generic?v.getProperty("program",""):"",version=generic?v.getProperty("version",""):"";
        if(generic&&(!TitlePolicy.genericProgram(program)||!TitlePolicy.genericVersionToken(version)))throw new IOException("Invalid native selection");
        String preparationPolicy=generic?v.getProperty("preparationPolicy",""):"";
        if(!preparationPolicy.isEmpty()&&!preparationPolicy.matches("[a-z0-9-]{1,64}"))throw new IOException("Invalid native preparation policy");
        String generation=uuid(v.getProperty("generation","")),source=v.getProperty("uri","");uri(source);
        // Keep stale generations readable for their document/save identity. They
        // cannot be launched/reused; the next preparation owns a fresh UUID.
        File root=new File(p,generation);validate(root,t,program);return new Selection(root,t,source,program,version,preparationPolicy);
    }
    static Selection publish(File files,String t,File stage,String source)throws IOException {return publish(files,t,stage,source,"","");}
    /** Generic games also record the prepared program ID and version token. */
    static Selection publish(File files,String t,File stage,String source,String program,String version)throws IOException {
        File p=parent(files,t);uri(source);boolean generic=TitlePolicy.generic(t);
        if(generic?!TitlePolicy.genericProgram(program)||!TitlePolicy.genericVersionToken(version):!program.isEmpty()||!version.isEmpty())
            throw new IOException("Native game identity differs");
        if(!stage.getAbsoluteFile().getParentFile().equals(p)||!stage.getName().startsWith(".incoming-")||Files.isSymbolicLink(stage.toPath()))throw new IOException("Unowned native stage");
        String generation=uuid(stage.getName().substring(10));validate(stage,t,program);
        File destination=new File(p,generation);if(destination.exists()||!stage.renameTo(destination))throw new IOException("Cannot publish native game files");
        Properties v=new Properties();v.setProperty("format","raitchi-native-package-v1");v.setProperty("title",t);v.setProperty("generation",generation);v.setProperty("uri",source);
        if(generic){v.setProperty("program",program);v.setProperty("version",version);v.setProperty("preparationPolicy",TitlePolicy.GENERIC_NATIVE_POLICY);}
        File temporary=new File(p,".selection-"+UUID.randomUUID());
        try {
            try(FileOutputStream out=new FileOutputStream(temporary)){v.store(out,"Raitchi native game document");out.getFD().sync();}
            Files.move(temporary.toPath(),new File(p,"selection.properties").toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{temporary.delete();}
        return new Selection(destination,t,source,program,version,generic?TitlePolicy.GENERIC_NATIVE_POLICY:"");
    }
    static void discard(File files,String t,File stage) {
        if(stage==null)return;try{File p=parent(files,t);if(!stage.getAbsoluteFile().getParentFile().equals(p)||!stage.getName().startsWith(".incoming-"))return;uuid(stage.getName().substring(10));remove(stage);}catch(IOException ignored){}
    }
    private static void remove(File f)throws IOException {
        if(Files.isSymbolicLink(f.toPath()))throw new IOException("Refusing symbolic native stage");
        if(f.isDirectory()){File[] children=f.listFiles();if(children==null)throw new IOException("Cannot inspect native stage");for(File child:children)remove(child);}
        if(f.exists()&&!f.delete())throw new IOException("Cannot remove native stage");
    }
    private NativePackageFiles(){}
}
