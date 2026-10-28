/* Actual snapshot admission/invalidation helpers, controlled renderer state. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn/maxwell/shader.h"
typedef struct NvnTexture { unsigned identity; } NvnTexture;
typedef struct { ShaderInfo *vi,*fi; } Prog;
static struct {unsigned char ds[8];uint32_t stencil_mask[2];struct{NvnTexture*depth;}rt;}st;
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static NvnTexture *binding[2][32];
static ShaderInfo *infos[2];
static NvnTexture *shader_texture_object(int stage,ShaderTexture*t){ptrdiff_t slot=t-infos[stage]->tex;CHECK(slot>=0&&slot<32);return binding[stage][slot];}
#include "production_snapshot_helpers.inc"
int main(void){
 NvnTexture depth={1},other={2};ShaderInfo vi={0},fi={0};Prog pr={&vi,&fi};infos[0]=&vi;infos[1]=&fi;st.rt.depth=&depth;
 CHECK(!snapshot_draw_writes_depth_stencil());CHECK(!snapshot_self_write_reuse(&pr,&depth));
 st.ds[0]=1;st.ds[1]=1;CHECK(snapshot_draw_writes_depth_stencil());CHECK(snapshot_self_write_reuse(&pr,&depth));CHECK(!snapshot_self_write_reuse(&pr,&other));
 st.ds[1]=0;st.ds[3]=1;st.stencil_mask[0]=0x40;CHECK(snapshot_draw_writes_depth_stencil()); /* actual Rift producer: depth-test on, depth-write off, stencil bit6 replace */
 st.stencil_mask[0]=0;st.stencil_mask[1]=0x20;CHECK(snapshot_draw_writes_depth_stencil());
 st.stencil_mask[1]=0x100;CHECK(!snapshot_draw_writes_depth_stencil());st.stencil_mask[1]=0;CHECK(!snapshot_draw_writes_depth_stencil());
 st.ds[3]=0;st.stencil_mask[0]=255;CHECK(!snapshot_draw_writes_depth_stencil());
 st.ds[0]=0;st.ds[3]=1;CHECK(snapshot_draw_writes_depth_stencil()); /* stencil can write with depth test disabled */
 st.ds[0]=1;st.ds[1]=1;fi.ntex=2;binding[1][0]=&depth;binding[1][1]=&depth;
 CHECK(snapshot_self_write_reuse(&pr,&depth));fi.tex[1].is_uint=true;CHECK(!snapshot_self_write_reuse(&pr,&depth)); /* float first, stencil second; no stale dedupe */
 fi.tex[1].is_uint=false;vi.ntex=1;vi.tex[0].is_uint=true;binding[0][0]=&depth;CHECK(!snapshot_self_write_reuse(&pr,&depth));
 binding[0][0]=&other;CHECK(snapshot_self_write_reuse(&pr,&depth));
 vi.ntex=0;fi.ntex=1;fi.tex[0].is_uint=true;CHECK(!snapshot_self_write_reuse(&pr,&depth));
 // Actual snapshot state: stencil-only producer invalidates its image but not unrelated copies.
 snap_tex[0]=&depth;snap_tex[1]=&other;snap_ok[0]=snap_ok[1]=true;st.ds[1]=0;st.ds[3]=1;st.stencil_mask[0]=0x40;snap_reason="stencil-only-producer";
 if(snapshot_draw_writes_depth_stencil())snapshots_invalidate(st.rt.depth);
 CHECK(!snap_ok[0]&&snap_ok[1]);CHECK(!strcmp(snap_why[0],"stencil-only-producer"));
 snap_ok[0]=true;st.ds[3]=0;if(snapshot_draw_writes_depth_stencil())snapshots_invalidate(st.rt.depth);CHECK(snap_ok[0]);
 snapshots_invalidate(NULL);CHECK(!snap_ok[0]&&!snap_ok[1]);
 printf("{\"state\":\"passed\",\"checks\":%u,\"actual_helpers\":true,\"controlled_renderer_state\":true}\n",checks);
}
