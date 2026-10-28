/* Asset-free integration harness for the ACTUAL shared Android loader overlay.
 * Includes production loader.c/current rt.h/common_session.h/CPU/manifest
 * types; does not duplicate/freeze production structs. Platform/HLE/dispatch
 * callbacks below are test fakes. SHA provider is deliberately a
 * constant/failure stub: these tests prove hash-result gating, NOT SHA256
 * cryptographic correctness. Synthetic NSOs live only in the checker-owned
 * build directory. argv: scenario (2|4) fixture-directory. Rejections exit42
 * only before commit; exit43 means a rejected input reached guest writes and is
 * a test failure.
 */

#include "ports/shared-android/runtime/loader.c"
#include <unistd.h>
static TitleManifest mf;
static NativeManifest native_mf;
static const uint8_t native_contract[32]={0x5a};
static bool native_only;
static unsigned dispatch_calls;
static TitleAdapter ad;
static ModuleAbiDescriptor desc;
static ModuleAbiModule mods[4];
static uint64_t addrs[4];
static ModuleAbiGuestFn fns[4];
static unsigned commits, host_calls;
static const uint8_t *guest_arena;
static size_t guest_arena_size;
static const char *mode;
static void fn(Cpu *c) {
  (void)c;
  abort();
}
const NativeManifest *shared_native_manifest_current(void) { return native_only ? &native_mf : NULL; }
const TitleManifest *shared_manifest_current(void) { return native_only ? NULL : &mf; }
const ModuleAbiDescriptor *shared_gamelib_descriptor(void) { return native_only ? NULL : &desc; }
const TitleAdapter *title_adapter_selected(void) { return &ad; }
bool shared_sha256_bytes(const void *p, size_t n, uint8_t out[32]) {
  (void)p;
  (void)n;
  memset(out, 0x77, 32);
  return strcmp(mode, "hash") && (strcmp(mode,"last-hash") || n<=0x40 || ((const uint8_t*)p)[0x40]!=4);
}
void plat_commit(uint64_t a, uint64_t n) {
  (void)a;
  (void)n;
  commits++;
}
void rt_fatal_msg(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vfprintf(stderr, fmt, a);
  va_end(a);
}
void rt_fatal_trace(void) {
  if (commits) _exit(43);
  for (size_t i = 0; i < guest_arena_size; ++i)
    if (guest_arena[i] != 0xa5) _exit(43);
  _exit(42);
}
GuestFn dispatch_lookup(uint64_t a) {
  dispatch_calls++;
  if (native_only) _exit(44); /* No generated dispatch admission in native mode. */
  for (unsigned i = 0; i < ad.module_count; i++)
    if (a == addrs[i])
      return fn;
  return NULL;
}
HostFn hle_lookup_impl(const char *s) {
  if (!strcmp(s, "self"))
    exit(31);
  if (!strcmp(s, "host") ||
      (!strcmp(mode, "host-peer") && !strcmp(s, "peer"))) {
    host_calls++;
    return fn;
  }
  return NULL;
}
uint64_t hle_register(const char *s, HostFn f) {
  (void)s;
  (void)f;
  return (native_only ? native_mf.identity.guest_window_hi : mf.profile.identity.guest_window_hi) + 0x1000;
}
/* Fail if unused host-definition registration is accidentally entered. */
GuestFn aot_dispatch_execution_backend(void) { _exit(45); }
int aot_dispatch_contains_text(uint64_t address) { (void)address; _exit(45); }
enum AotStatus aot_dispatch_override(uint64_t address, GuestFn function) {
  (void)address; (void)function; _exit(45);
}
#define UNUSED_MEMORY(name) void name(Cpu *cpu) { (void)cpu; _exit(45); }
UNUSED_MEMORY(hle_memcpy) UNUSED_MEMORY(hle_memmove) UNUSED_MEMORY(hle_memset)
UNUSED_MEMORY(hle_memcmp) UNUSED_MEMORY(hle_bcmp) UNUSED_MEMORY(hle_strlen)
UNUSED_MEMORY(hle_strcmp) UNUSED_MEMORY(hle_strncmp) UNUSED_MEMORY(hle_memchr)
#undef UNUSED_MEMORY
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void dyn(uint8_t *b, unsigned *n, uint64_t t, uint64_t v) {
  w64(b + 0x100 + 16 * *n, t);
  w64(b + 0x108 + 16 * (*n)++, v);
}
static bool relr_success_case(void) {
  return !strcmp(mode,"relr-valid") || !strcmp(mode,"relr-last-valid") ||
      !strcmp(mode,"relr-empty") || !strcmp(mode,"relr-only") ||
      !strcmp(mode,"relr-empty-bitmap") || !strcmp(mode,"relr-max-addend") ||
      !strcmp(mode,"relr-metadata-snapshot") || !strcmp(mode,"relr-none-overlap") ||
      !strcmp(mode,"gnu-relr-valid");
}
static bool gnu_success_case(void) {
  const char *cases[]={"gnu-valid","gnu-all-valid","gnu-last-valid","gnu-empty",
      "gnu-zero-buckets","gnu-repeat","gnu-coexist","gnu-sysv-authoritative",
      "gnu-metadata-snapshot","gnu-relr-valid"};
  for(size_t i=0;i<sizeof cases/sizeof *cases;i++)if(!strcmp(mode,cases[i]))return true;
  return false;
}
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  mode = argv[1];
  native_only = !strncmp(mode, "native-", 7);
  if (native_only) mode += 7;
  unsigned count = (unsigned)atoi(argv[2]);
  if ((count != 2 && count != 4) || (native_only && count != 4)) return 2;
  unsigned long long rejected_tag = 0, rejected_value = 0;
  unsigned rejected_module = 0;
  bool rejected_tag_case = !strncmp(mode, "reject-tag-", 11);
  if (rejected_tag_case) {
    int consumed = 0;
    if (sscanf(mode, "reject-tag-%llx-%llx-%u%n", &rejected_tag, &rejected_value,
               &rejected_module, &consumed) != 3 || mode[consumed] ||
        rejected_module >= count) return 2;
  }
  uint8_t *arena = aligned_alloc(16384, count * 16384);
  if (!arena)
    return 2;
  memset(arena, 0xa5, count * 16384);
  guest_arena = arena;
  guest_arena_size = count * 16384;
  uint64_t base = (uintptr_t)arena;
  ad.id = native_only ? TITLE_ECHOES : count == 2 ? TITLE_LAS : TITLE_EEVEE;
  ad.module_count = count;
  ModuleAbiIdentity identity={.struct_size=sizeof(ModuleAbiIdentity),.abi_version=MODULE_ABI_VERSION,
      .target=MODULE_ABI_TARGET_ANDROID_ARM64_LP64,.cpu_identity=MODULE_ABI_CPU_A64_SHARED_66560,
      .cpu_size=sizeof(Cpu),.cpu_alignment=_Alignof(Cpu),.guest_window_lo=base,
      .guest_window_hi=base+count*16384};
  if (native_only) {
    native_mf=(NativeManifest){.struct_size=sizeof native_mf,.version=NATIVE_MANIFEST_VERSION,
      .title_id=TITLE_ECHOES,.program_id=UINT64_C(0x01008cf01baac000),.module_count=count,
      .mapping_alignment=16384,.identity=identity};
    memcpy(native_mf.contract_sha256,native_contract,32);
    ad.program_id=native_mf.program_id;
  } else {
    mf.title_id=ad.id;
    mf.profile=(ModuleAbiProfile){.struct_size=sizeof(ModuleAbiProfile),.module_count=count,
      .identity=identity,.modules=mf.modules};
    desc=(ModuleAbiDescriptor){.struct_size=sizeof desc,.module_count=count,
      .identity=identity,.modules=mods};
  }
  const char *aot_names[]={"main","sdk","subsdk0","subsdk1"};
  const char *native_names[]={"main","subsdk0","subsdk1","sdk"};
  const char **names=native_only?native_names:aot_names;
  const uint32_t native_ids[]={MODULE_ABI_MAIN,MODULE_ABI_SUBSDK0,MODULE_ABI_SUBSDK1,MODULE_ABI_SDK};
  bool relr_case = !strncmp(mode,"relr-",5) || !strcmp(mode,"gnu-relr-valid");
  bool gnu_case = !strncmp(mode,"gnu-",4);
  unsigned relr_module = (!strcmp(mode,"relr-last-valid") || !strcmp(mode,"relr-last-bad")) ? count-1 : 0;
  for (unsigned i = 0; i < count; i++) {
    uint8_t im[0x2000] = {0}, raw[0xe00] = {0};
    bool relr_here = relr_case && i == relr_module;
    bool gnu_here = gnu_case && (!strcmp(mode,"gnu-all-valid") ||
        ((!strcmp(mode,"gnu-last-valid") || !strcmp(mode,"gnu-last-bad")) ? i==count-1 : i==0));
    unsigned gnu_count=i?2:!strcmp(mode,"gnu-relr-valid")?5:7;
    uint64_t gnu_off=0x1200, gnu_strtab=gnu_count==7?0x1100:0x3c0, gnu_symtab=0x340;
    if (!strcmp(mode,"gnu-header-unaligned"))gnu_off=0x1204;
    if (!strcmp(mode,"gnu-header-unbacked"))gnu_off=0x1400;
    if (!strcmp(mode,"gnu-header-truncated"))gnu_off=0x13f8;
    if (!strcmp(mode,"gnu-bloom-truncated"))gnu_off=0x13e0;
    if (!strcmp(mode,"gnu-buckets-truncated"))gnu_off=0x13d8;
    if (!strcmp(mode,"gnu-chain-unbacked"))gnu_off=0x13d0;
    if (!strcmp(mode,"gnu-chain-dynsym-capacity"))gnu_strtab=0x3e8;
    if (!strcmp(mode,"gnu-symbol-unaligned"))gnu_symtab=0x341;
    if (!strcmp(mode,"gnu-symbol-unbacked"))gnu_symtab=0x1400;
    uint64_t relr_table[70] = {0x1080, (UINT64_C(1)<<63)|3, 3, 0x1300, 0x1400};
    size_t relr_words = 5;
    uint64_t relr_off = 0x1100, relr_entry_size = 8;
    uint32_t data_size = relr_here && !strcmp(mode,"relr-count-limit") ? 0x800 : 0x400;
    if (relr_here) {
      if (!strcmp(mode,"relr-empty")) relr_words=0;
      if (!strcmp(mode,"relr-empty-bitmap")) relr_table[1]=1;
      if (!strcmp(mode,"relr-only")) {relr_table[0]=0x1010;relr_table[1]=0x1018;relr_words=native_only?2:1;}
      if (!strcmp(mode,"relr-metadata-snapshot")) {
        relr_table[0]=0x1108;relr_table[1]=0x1200;relr_table[2]=0x360;relr_table[3]=0x400;relr_words=4;
      }
      if (!strcmp(mode,"relr-bad-entsize")) relr_entry_size=16;
      if (!strcmp(mode,"relr-table-unaligned")) relr_off=0x1101;
      if (!strcmp(mode,"relr-table-unbacked")) relr_off=0x1400;
      if (!strcmp(mode,"relr-table-cross-end")) {relr_off=0x13f8;relr_words=2;}
      if (!strcmp(mode,"relr-table-rela-overlap")) {relr_off=0x400;relr_words=1;}
      if (!strcmp(mode,"relr-table-plt-overlap")) {relr_off=0x1300;relr_words=1;}
      if (!strcmp(mode,"relr-bitmap-first")) {relr_table[0]=3;relr_words=1;}
      if (!strcmp(mode,"relr-target-unaligned")) {relr_table[0]=0x1082;relr_words=1;}
      if (!strcmp(mode,"relr-target-gap")) {relr_table[0]=0x800;relr_words=1;}
      if (!strcmp(mode,"relr-target-padding")) {relr_table[0]=0x2000;relr_words=1;}
      if (!strcmp(mode,"relr-target-outside") || !strcmp(mode,"relr-last-bad")) {relr_table[0]=0x4000;relr_words=1;}
      if (!strcmp(mode,"relr-cursor-overflow")) {relr_table[0]=UINT64_MAX-7;relr_words=1;}
      if (!strcmp(mode,"relr-addend-overflow")) relr_words=1;
      if (!strcmp(mode,"relr-duplicate")) {relr_table[1]=relr_table[0];relr_words=2;}
      if (!strcmp(mode,"relr-bitmap-duplicate")) {relr_table[1]=3;relr_table[2]=0x1088;relr_words=3;}
      if (!strcmp(mode,"relr-rela-overlap")) {relr_table[0]=0x1030;relr_words=1;}
      if (!strcmp(mode,"relr-plt-overlap")) relr_words=1;
      if (!strcmp(mode,"relr-count-limit")) {
        relr_off=0x1500;relr_words=66;
        for(size_t r=0;r<relr_words;r+=2) {relr_table[r]=0x1000;relr_table[r+1]=UINT64_MAX>>1;}
      }
    }
    unsigned n = 0;
    w32(im + 4, 0x80);
    memcpy(im + 0x80, "MOD0", 4);
    w32(im + 0x84, 0x80);
    w32(im + 0x88, 0xf80+data_size);
    w32(im + 0x8c, 0x1380+data_size);
    if (!gnu_here || !strcmp(mode,"gnu-coexist") || !strcmp(mode,"gnu-sysv-authoritative")) dyn(im, &n, DT_HASH, 0x300);
    if (gnu_here && strcmp(mode,"gnu-missing-hash")) {
      dyn(im,&n,DT_GNU_HASH,!strcmp(mode,"gnu-sysv-authoritative")?UINT64_MAX:gnu_off);
      if (!strcmp(mode,"gnu-repeat") || !strcmp(mode,"gnu-conflicting-tag"))
        dyn(im,&n,DT_GNU_HASH,gnu_off+(!strcmp(mode,"gnu-conflicting-tag")?8:0));
    }
    dyn(im, &n, DT_SYMTAB, gnu_here?gnu_symtab:0x340);
    dyn(im, &n, DT_SYMENT, gnu_here && !strcmp(mode,"gnu-bad-syment")?16:24);
    dyn(im, &n, DT_STRTAB, gnu_here?gnu_strtab:0x3c0);
    dyn(im, &n, DT_STRSZ, gnu_here && gnu_count==7?31:21);
    dyn(im, &n, DT_RELA, 0x400);
    dyn(im, &n, DT_RELASZ, relr_here && !strcmp(mode,"relr-only") ? 0 : native_only && !strcmp(mode,"late-relocation") && i==count-1 ? 24 : i ? 0 : native_only ? 192 : 144);
    dyn(im, &n, DT_RELAENT, 24);
    dyn(im, &n, DT_INIT, native_only ? (!strcmp(mode,"init-outside") && !i ? 0x200 : !strcmp(mode,"init-unaligned") && !i ? 0x42 : 0x44) : 0x40);
    if (native_only) dyn(im,&n,DT_FINI,(!strcmp(mode,"fini-outside") && !i)||(!strcmp(mode,"last-fini-outside") && i==count-1) ? 0x1000 : 0x4c);
    dyn(im, &n, DT_INIT_ARRAY, 0x1010);
    dyn(im, &n, DT_INIT_ARRAYSZ, i ? 0 : 8);
    if (native_only) {dyn(im,&n,DT_FINI_ARRAY,0x1018);dyn(im,&n,DT_FINI_ARRAYSZ,!strcmp(mode,"last-array-outside") && i==count-1 ? 8 : i?0:8);}
    if (i == 0 && !strcmp(mode, "packed"))
      dyn(im, &n, 0x60000011, 0x400);
    if (rejected_tag_case && i == rejected_module)
      dyn(im, &n, rejected_tag, rejected_value);
    if (relr_here) {
      if (strcmp(mode,"relr-missing-address")) dyn(im,&n,DT_RELR,relr_off);
      if (strcmp(mode,"relr-missing-size")) dyn(im,&n,DT_RELRSZ,
          !strcmp(mode,"relr-bad-size") ? 9 : !strcmp(mode,"relr-table-count-limit") ? UINT64_MAX-7 : relr_words*8);
      if (strcmp(mode,"relr-missing-entsize")) dyn(im,&n,DT_RELRENT,relr_entry_size);
      if (!strcmp(mode,"relr-conflicting-tag")) dyn(im,&n,DT_RELR,relr_off+8);
      if (!strcmp(mode,"relr-plt-overlap") || !strcmp(mode,"relr-table-plt-overlap")) {
        dyn(im,&n,DT_JMPREL,0x1300);dyn(im,&n,DT_PLTRELSZ,24);dyn(im,&n,DT_PLTREL,DT_RELA);
      }
    }
    dyn(im, &n, DT_NULL, 0);
    w32(im + 0x300, 1);
    w32(im + 0x304, gnu_here ? gnu_count : i ? 2 : 5);
    memcpy(im + 0x3c0, "\0self\0peer\0host\0weak\0", 21);
    Elf64Sym syms[7] = {
        {0},
        {.name = i ? 6 : 1, .info = 0x12, .shndx = 1, .value = 0x40, .size = 4},
        {.name = 6, .info = 0x12},
        {.name = 11, .info = 0x12},
        {.name = 16, .info = 0x22}};
    Elf64Rela rs[8] = {
        {.off = 0x1010, .info = R_RELATIVE, .addend = 0x40},
        {.off = 0x1030, .info = (UINT64_C(2) << 32) | R_ABS64, .addend = 4},
        {.off = 0x1040, .info = (UINT64_C(3) << 32) | R_GLOB_DAT},
        {.off = 0x1048, .info = (UINT64_C(4) << 32) | R_ABS64, .addend = 123},
        {.off = 0x1050, .info = (UINT64_C(1) << 32) | R_ABS64},
        {.off = 0x1058, .info = R_NONE}};
    if (native_only) {
      rs[0]=(Elf64Rela){.off=0x1010,.info=(UINT64_C(2)<<32)|R_ABS64,.addend=0x10};
      rs[6]=(Elf64Rela){.off=0x1018,.info=R_RELATIVE,.addend=0x60};
      rs[7]=(Elf64Rela){.off=0x1060,.info=(UINT64_C(2)<<32)|R_JUMP_SLOT};
      if(!i && !strcmp(mode,"array-hle"))rs[0].info=(UINT64_C(3)<<32)|R_GLOB_DAT;
      if(!i && !strcmp(mode,"fini-array-outside"))rs[6].addend=0x1000;
      if(!i && !strcmp(mode,"fini-array-unaligned"))rs[6].addend=0x42;
      if(i==count-1 && !strcmp(mode,"last-array-outside"))w64(im+0x1018,base+i*16384+0x1000);
      if(i==count-1 && !strcmp(mode,"late-relocation"))rs[0].info=999;
    }
    if (gnu_here && !i) {
      /* GNU hash indexes definitions after the undefined-symbol prefix. */
      Elf64Sym self=syms[1];syms[1]=syms[2];syms[2]=syms[3];syms[3]=syms[4];syms[4]=self;
      if(gnu_count==7) {
        syms[5]=(Elf64Sym){.name=21,.info=0x12,.shndx=1,.value=0x40,.size=4};
        syms[6]=(Elf64Sym){.name=26,.info=0x12,.shndx=1,.value=0x40,.size=4};
      }
      if(!strcmp(mode,"gnu-empty") || !strcmp(mode,"gnu-zero-buckets"))
        for(unsigned s=4;s<gnu_count;s++)syms[s].info=2; /* Unindexed local definitions. */
      for(size_t r=0;r<sizeof rs/sizeof *rs;r++) {
        uint64_t symbol=rs[r].info>>32;
        if(symbol)rs[r].info=((symbol==1?4:symbol-1)<<32)|(uint32_t)rs[r].info;
      }
      if(!strcmp(mode,"gnu-strong"))memcpy(im+gnu_strtab+6,"lost",4);
      if(!strcmp(mode,"gnu-name"))syms[1].name=999;
      if(!strcmp(mode,"gnu-symbol"))rs[1].info=(UINT64_C(99)<<32)|R_ABS64;
      if(!strcmp(mode,"gnu-metadata-snapshot"))rs[5]=(Elf64Rela){.off=0x1200,.info=R_RELATIVE,.addend=0x40};
    }
    if (i == 0) {
      if (!strcmp(mode,"relr-partial-before")) rs[1].off=0x1079;
      if (!strcmp(mode,"relr-partial-after")) rs[1].off=0x1087;
      if (!strcmp(mode,"relr-none-overlap")) rs[5].off=0x1080;
      if (!strcmp(mode, "unknown"))
        rs[1].info = 999;
      if (!strcmp(mode, "destination"))
        rs[1].off = 16380;
      if (!strcmp(mode, "symbol"))
        rs[1].info = (UINT64_C(99) << 32) | R_ABS64;
      if (!strcmp(mode, "strong"))
        memcpy(im + 0x3c6, "lost", 4);
      if (!strcmp(mode, "name"))
        syms[2].name = 999;
      if (!strcmp(mode, "mod0"))
        im[0x80] = 'X';
      if (!strcmp(mode, "dynamic"))
        w32(im + 0x84, 0x1780);
      if (!strcmp(mode, "array"))
        rs[0].addend = native_only ? 0x200 : 0x44;
    }
    memcpy(im + 0x340, syms, (gnu_here ? gnu_count : i ? 2 : 5) * 24);
    memcpy(im + 0x400, rs, native_only ? sizeof rs : 6*sizeof *rs);
    if (relr_here) {
      w64(im+0x1080,!strcmp(mode,"relr-addend-overflow")?UINT64_MAX:0x24);
      if (!strcmp(mode,"relr-max-addend")) w64(im+0x1080,UINT64_MAX-(base+i*16384));
      w64(im+0x1088,0);w64(im+0x1278,0x80);w64(im+0x1280,0x1500);w64(im+0x1300,0x40);
      if (!strcmp(mode,"relr-only")) {w64(im+0x1010,0x40);w64(im+0x1018,0x60);}
      if (!strcmp(mode,"relr-metadata-snapshot")) w64(im+0x1200,0x18);
      /* An invalid table must not overwrite unrelated fixture metadata. */
      if (relr_off>=0x1000 && relr_off+relr_words*8<=0x1000+data_size)
        memcpy(im+relr_off,relr_table,relr_words*8);
      if (!strcmp(mode,"relr-plt-overlap") || !strcmp(mode,"relr-table-plt-overlap")) {
        Elf64Rela plt={.off=0x1080,.info=R_RELATIVE,.addend=0x40};memcpy(im+0x1300,&plt,sizeof plt);
      }
    }
    if (gnu_here) {
      memcpy(im+gnu_strtab,"\0self\0peer\0host\0weak\0tail\0more\0",gnu_count==7?31:21);
      if(!strcmp(mode,"gnu-strong"))memcpy(im+gnu_strtab+6,"lost",4);
      uint32_t header[]={2,i?1:4,2,26};
      uint32_t buckets[]={i?1:4,gnu_count==7?6:0};
      uint32_t chains[]={2,5,7};
      if(gnu_count!=7)chains[0]=3;
      if(!strcmp(mode,"gnu-empty") || !strcmp(mode,"gnu-zero-buckets")) {
        header[1]=gnu_count;buckets[0]=buckets[1]=0;
        if(!strcmp(mode,"gnu-zero-buckets"))header[0]=0;
      }
      if(!strcmp(mode,"gnu-bloom-zero"))header[2]=0;
      if(!strcmp(mode,"gnu-bloom-not-power2"))header[2]=3;
      if(!strcmp(mode,"gnu-bloom-cap"))header[2]=UINT32_C(1)<<31;
      if(!strcmp(mode,"gnu-shift"))header[3]=32;
      if(!strcmp(mode,"gnu-buckets-cap"))header[0]=UINT32_MAX;
      if(!strcmp(mode,"gnu-buckets-truncated"))header[0]=3;
      if(!strcmp(mode,"gnu-symoffset-zero"))header[1]=0;
      if(!strcmp(mode,"gnu-symoffset-capacity"))header[1]=UINT32_MAX;
      if(!strcmp(mode,"gnu-bucket-prefix"))buckets[0]=3;
      if(!strcmp(mode,"gnu-bucket-capacity"))buckets[1]=UINT32_MAX;
      if(!strcmp(mode,"gnu-chain-boundary"))buckets[1]=5;
      if(!strcmp(mode,"gnu-chain-unterminated") || !strcmp(mode,"gnu-last-bad") || !strcmp(mode,"gnu-chain-dynsym-capacity"))
        memset(chains,0,sizeof chains);
      memcpy(im+gnu_off,header,sizeof header);
      /* Fixture dimensions stay bounded even when the advertised ones are corrupt. */
      w64(im+gnu_off+16,UINT64_MAX);w64(im+gnu_off+24,UINT64_MAX);
      memcpy(im+gnu_off+32,buckets,sizeof buckets);memcpy(im+gnu_off+40,chains,sizeof chains);
      if(!strcmp(mode,"gnu-chain-dynsym-capacity"))w32(im+gnu_off+52,1);
    }
    memcpy(raw, "NSO0", 4);
    for (unsigned b = 0; b < 32; b++)
      raw[0x40 + b] = (uint8_t)(i + b + 1);
    const uint32_t fo[] = {0x100, 0x200, 0x600}, mo[] = {0, 0x100, 0x1000},
                   sz[] = {0x100, 0x400, data_size};
    for (unsigned s = 0; s < 3; s++) {
      w32(raw + 0x10 + s * 16, fo[s]);
      w32(raw + 0x14 + s * 16, mo[s]);
      w32(raw + 0x18 + s * 16, sz[s]);
      w32(raw + 0x60 + s * 4, sz[s]);
      memcpy(raw + fo[s], im + mo[s], sz[s]);
    }
    w32(raw + 0x3c, 0x400);
    NativeModuleSpec original={.struct_size=sizeof original,
      .module_id=native_only?native_ids[i]:i+1,.canonical_base=base+i*16384,
      .mapped_size=16384,.text_size=0x100};
    memcpy(original.nso_build_id,raw+0x40,32);memset(original.nso_sha256,0x77,32);
    if (i == 0 && !strcmp(mode, "order")) {
      uint8_t segment[12];memcpy(segment,raw+0x10,sizeof segment);
      memcpy(raw+0x10,raw+0x20,sizeof segment);memcpy(raw+0x20,segment,sizeof segment);
      w32(raw+0x60,0x400);w32(raw+0x64,0x100);original.text_offset=0x100;original.text_size=0x400;
    }
    if(native_only) {
      if((!i && !strcmp(mode,"decode"))||(i==count-1&&!strcmp(mode,"last-decode")))w32(raw+0x10,0xff00);
      if(!i && !strcmp(mode,"build"))raw[0x40]^=1;
      if(!i && !strcmp(mode,"range"))original.text_size=0x104;
      native_mf.modules[i]=original;
    } else {
      ModuleAbiModuleSpec *spec=&mf.modules[i];
      *spec=(ModuleAbiModuleSpec){.struct_size=sizeof *spec,.module_id=original.module_id,
        .canonical_base=original.canonical_base,.mapped_size=original.mapped_size,
        .text_offset=original.text_offset,.text_size=original.text_size,.function_count=1};
      memcpy(spec->nso_build_id,original.nso_build_id,32);memcpy(spec->nso_sha256,original.nso_sha256,32);
      addrs[i]=spec->canonical_base+spec->text_offset+0x40;fns[i]=fn;
      mods[i]=(ModuleAbiModule){.struct_size=sizeof(ModuleAbiModule),.image=*spec,
        .address_count=1,.addresses=&addrs[i],.functions=&fns[i]};
    }
    ad.modules[i]=(TitleModulePolicy){original.module_id,names[i],original.canonical_base};
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", argv[3], names[i]);
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(raw, 1, sizeof raw, file) != sizeof raw || fclose(file))
      return 2;
  }
  if(native_only) {
    NativeManifestFailure failure;
    if(native_manifest_validate(&native_mf,native_contract,&failure)!=NATIVE_MANIFEST_OK)return 10;
    if(shared_manifest_current()!=NULL || shared_gamelib_descriptor()!=NULL || desc.modules || mf.profile.modules)return 11;
  }
  loader_load(argv[3]);
  if (strcmp(mode, "valid") && strcmp(mode, "host-peer") && !relr_success_case() && !gnu_success_case())
    return 3;
  if(gnu_success_case()) {
    unsigned module=!strcmp(mode,"gnu-last-valid")?count-1:0;
    if(g_modules[module].nsyms!=(module?2:!strcmp(mode,"gnu-relr-valid")?5:7))return 20;
    if(!strcmp(mode,"gnu-metadata-snapshot") && read64(arena+0x1200)!=base+0x40)return 21;
  }
  uint64_t value;
  if (relr_success_case()) {
    uint64_t relr_base=base+relr_module*16384;
    uint8_t *image=arena+relr_module*16384;
    if (!strcmp(mode,"relr-only")) {
      if(read64(image+0x1010)!=relr_base+0x40 ||
          (native_only && read64(image+0x1018)!=relr_base+0x60) || commits!=count)return 16;
      if(native_only)printf("generated_dispatch_calls=0; ");
      printf("RELR-only init/fini arrays pass\n");return 0;
    }
    if (!strcmp(mode,"relr-metadata-snapshot")) {
      if(read64(image+0x1108)!=relr_base+0x1200 || read64(image+0x1200)!=relr_base+0x18 ||
          read64(image+0x360)!=relr_base+0x40 || read64(image+0x400)!=relr_base+0x1010 ||
          loader_find_symbol("self")!=base+0x40)return 17;
    } else if (strcmp(mode,"relr-empty")) {
      bool empty_bitmap=!strcmp(mode,"relr-empty-bitmap");
      uint64_t first=!strcmp(mode,"relr-max-addend")?UINT64_MAX:relr_base+0x24;
      if(read64(image+0x1080)!=first || read64(image+0x1088)!=(empty_bitmap?0:relr_base) ||
          read64(image+0x1278)!=(empty_bitmap?0x80:relr_base+0x80) || read64(image+0x1280)!=relr_base+0x1500 ||
          read64(image+0x1300)!=relr_base+0x40 || read64(image+0x1400)!=relr_base)return 18;
      /* Unset bitmap slots remain untouched, including initialized and BSS bytes. */
      if(read64(image+0x1090) || read64(image+0x1408))return 19;
    }
  }
  memcpy(&value, arena + 0x1010, 8);
  if (value != base + (native_only ? 16384+0x50 : 0x40))
    return 4;
  memcpy(&value, arena + 0x1030, 8);
  uint64_t want = !strcmp(mode, "host-peer")
                      ? mf.profile.identity.guest_window_hi + 0x1004
                      : base + 16384 + 0x44;
  if (value != want)
    return 5;
  memcpy(&value, arena + 0x1048, 8);
  if (value)
    return 6;
  memcpy(&value, arena + 0x1050, 8);
  if (value != base + 0x40)
    return 7;
  if (commits != count || g_nmodules != (int)count ||
      loader_find_symbol("peer") != base + 16384 + 0x40 ||
      host_calls != (unsigned)(!strcmp(mode, "host-peer") ? 2 : 1))
    return 8;
  uint64_t off;
  const char *name = loader_symbol_at(base + 0x41, &off);
  if (!name || strcmp(name, "self") || off != 1)
    return 9;
  if(native_only) {
    memcpy(&value,arena+0x1018,8);if(value!=base+0x60)return 12;
    memcpy(&value,arena+0x1060,8);if(value!=base+16384+0x40)return 13;
    if(dispatch_calls || shared_manifest_current()!=NULL || shared_gamelib_descriptor()!=NULL)return 14;
    for(unsigned i=0;i<count;i++)if(g_modules[i].init!=base+i*16384+0x44 || g_modules[i].fini!=base+i*16384+0x4c || strcmp(g_modules[i].name,native_names[i]))return 15;
    printf("native-only descriptor=NULL generated_dispatch_calls=0 IDs=1,3,4,2; ");
  }
  printf("%u modules %s: cross-module/HLE/self/weak/arrays/post-copy symbols "
         "pass\n",
         count, mode);
  return 0;
}
