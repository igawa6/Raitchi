/* Real ImGui geometry lifetime: idle FPS overlay, title/input/menu transitions,
 * transient-window collection and immediate same-frame redraw. No GPU calls. */
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned checks,nonempty;
#define CHECK(x) do{checks++;if(!(x)){std::fprintf(stderr,"line%d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void copy_draw_data(ImDrawData *data){
 CHECK(data&&data->Valid);int vertices=0,indices=0;
 std::vector<ImDrawVert> v((size_t)data->TotalVtxCount);std::vector<ImDrawIdx> ix((size_t)data->TotalIdxCount);
 for(const ImDrawList*list:data->CmdLists){CHECK(list);CHECK(list->VtxBuffer.Size==0||list->VtxBuffer.Data);CHECK(list->IdxBuffer.Size==0||list->IdxBuffer.Data);if(list->VtxBuffer.Size)std::memcpy(v.data()+vertices,list->VtxBuffer.Data,(size_t)list->VtxBuffer.Size*sizeof(ImDrawVert));if(list->IdxBuffer.Size)std::memcpy(ix.data()+indices,list->IdxBuffer.Data,(size_t)list->IdxBuffer.Size*sizeof(ImDrawIdx));vertices+=list->VtxBuffer.Size;indices+=list->IdxBuffer.Size;}
 CHECK(vertices==data->TotalVtxCount&&indices==data->TotalIdxCount);CHECK((vertices==0)==(indices==0));if(vertices)nonempty++;
}
int main(){
 ImGui::CreateContext();ImGuiIO&io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1920,1080);io.DeltaTime=1.0f/60;io.ConfigMemoryCompactTimer=0.1f;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;unsigned char*pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);CHECK(pixels&&w>0&&h>0);
 for(unsigned frame=0;frame<900;frame++){
  const bool menu=(frame>=240&&frame<300)||(frame>=600&&frame<660);const bool title=(frame>=300&&frame<480);
  io.AddKeyEvent(ImGuiKey_Enter,frame==240||frame==600);ImGui::NewFrame();CHECK(ImGui::GetFrameCount()==(int)frame+1);CHECK(!ImGui::GetDrawData());
  ImGui::SetNextWindowPos(ImVec2(1700,16),ImGuiCond_Always);ImGui::Begin("##fps",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize);ImGui::TextUnformatted("60 fps");ImGui::End();
  if(title){ImGui::Begin("title choices");ImGui::Button("Continue");ImGui::Button("New game");ImGui::End();}
  if(menu){ImGui::Begin("game settings");ImGui::TextUnformatted("Settings after idle");ImGui::Button("Resume");ImGui::End();}
  ImGui::Render();ImDrawData*data=ImGui::GetDrawData();copy_draw_data(data);copy_draw_data(data);CHECK(data==ImGui::GetDrawData());
 }
 CHECK(nonempty>1700);ImGui::DestroyContext();std::printf("{\"state\":\"passed\",\"checks\":%u,\"frames\":900,\"real_imgui_geometry_copies\":1800,\"idle_menu_transitions\":true}\n",checks);return 0;
}
