package com.lasrecomp.android;

/** Shared shell identity and module inventory; native adapters validate actual builds. */
final class TitlePolicy {
    static final String[] TITLES = {"las", "eevee", "echoes", "wonder"};
    final int id; final String label; final boolean nativeOnly, experimental, renderScaling; final String[] modules;
    private TitlePolicy(int id,String label,boolean nativeOnly,boolean experimental,boolean renderScaling,String... modules) {
        this.id=id;this.label=label;this.nativeOnly=nativeOnly;this.experimental=experimental;this.renderScaling=renderScaling;this.modules=modules;
    }
    private static final TitlePolicy LAS=new TitlePolicy(1,"Link’s Awakening",false,false,true,"main","sdk");
    private static final TitlePolicy EEVEE=new TitlePolicy(2,"Let’s Go Eevee",false,false,false,"main","subsdk0","subsdk1","sdk");
    private static final TitlePolicy ECHOES=new TitlePolicy(3,"Echoes of Wisdom",true,false,false,"main","subsdk0","subsdk1","sdk");
    private static final TitlePolicy WONDER=new TitlePolicy(4,"Super Mario Bros. Wonder (experimental)",true,true,false,"main","sdk");
    /** Generic startup for a game without an adapter. Not listed in TITLES; the
     *  module set (main, sdk, optional subsdk0/subsdk1) comes from the package. */
    static final String GENERIC="generic";
    /** Generic manifest policy: own a 16 MiB bank below unchanged guest images.
     * Bump when generic native preparation identity changes; saves keep their
     * program/version namespace independently of this preparation revision. */
    static final String GENERIC_NATIVE_POLICY="lead-in-bank-v2";
    private static final TitlePolicy GENERIC_POLICY=new TitlePolicy(5,"Untested game (generic startup)",true,true,false);
    static TitlePolicy get(String title) {
        if("las".equals(title))return LAS;
        if("eevee".equals(title))return EEVEE;
        if("echoes".equals(title))return ECHOES;
        if("wonder".equals(title))return WONDER;
        if(GENERIC.equals(title))return GENERIC_POLICY;
        throw new IllegalArgumentException("Unsupported title");
    }
    static boolean generic(String title) {return GENERIC.equals(title);}
    /** Same rule as title_generic_version_token in native/adapters/generic/generic.c. */
    static String genericVersion(String version) {
        StringBuilder out=new StringBuilder();
        if(version!=null)for(int i=0;i<version.length()&&out.length()<16;) {
            int c=version.codePointAt(i);i+=Character.charCount(c);
            // Native code works on UTF-8 bytes: one replacement per encoded byte.
            int bytes=c<0x80?1:c<0x800?2:c<0x10000?3:4;
            boolean plain=(c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='.'||c=='_'||c=='-';
            for(int b=0;b<bytes&&out.length()<16;b++)out.append(plain?(char)c:'_');
        }
        return out.length()==0?"0":out.toString();
    }
    /** Base application program ID as 16 lowercase hex digits (low 12 bits zero). */
    static boolean genericProgram(String hex) {return hex!=null&&hex.matches("[0-9a-f]{13}000")&&!"0000000000000000".equals(hex);}
    static boolean genericVersionToken(String version) {return version!=null&&version.matches("[0-9A-Za-z._-]{1,16}");}
    static String genericNamespace(String programHex,String version) {return GENERIC+"-"+programHex+"-"+version;}
    static boolean nativeOnly(String title) {return title!=null&&get(title).nativeOnly;}
    static boolean supportsRenderScaling(String title) {return title==null||get(title).renderScaling;}
}
