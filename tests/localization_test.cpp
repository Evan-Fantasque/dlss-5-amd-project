#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_dx11.h>
#include "ui_localization.h"
#include "native_menu_hints.h"
#include <imgui/imgui_internal.h>
#include <map>
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
struct Rect { ImVec2 a,b; };
static std::map<std::string,Rect> items;
void RecordItem(const char* id) { items[id]={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()}; }
static void Check(bool ok,const char* label) { if(!ok) throw std::runtime_error(label); std::cout<<"PASS "<<label<<"\n"; }
int main(int argc, char** argv) {
try {
    const std::filesystem::path output = argc > 1 ? argv[1] : ".build/menu-preview";
    std::filesystem::create_directories(output);
    std::filesystem::current_path(output);
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx)),"WARP device");
    const UINT width=1120,height=900;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> rtv;
    Check(SUCCEEDED(dev->CreateTexture2D(&desc,nullptr,&target)) && SUCCEEDED(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"offscreen target");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(width,height);io.DeltaTime=1.f/60;io.IniFilename=nullptr;
    ImGui::StyleColorsDark();
    char windows[MAX_PATH]{};GetWindowsDirectoryA(windows,MAX_PATH);
    const auto fontPath=(std::filesystem::path(windows)/"Fonts/msyh.ttc").string();
    auto font=io.Fonts->AddFontFromFileTTF(fontPath.c_str(),18.f);
    Check(font!=nullptr,"Chinese font");
    Check(ImGui_ImplDX11_Init(dev.Get(),ctx.Get()),"DX11 renderer");
    MenuI18n::enabled=true;
    Check(std::string(MenuI18n::Text("Enable Neural Rendering")).find("神经")!=std::string::npos,"Chinese catalog lookup");
    Check(std::string(MenuI18n::Text("not in catalog"))=="not in catalog","unknown names remain unchanged");
    Check(std::string(MenuI18n::Label("##hidden"))=="##hidden","hidden identifiers remain unchanged");
    bool nr=true,pre=false,fg=false,overrideRatio=true,drsMin=true,drsMax=true;
    float scale=75,tone=1,structure=1,skin=-1,ratio=1.3f;int passes=1,backend=0;
    bool save=false,close=false;
    float viewScale=1.f;
    ImVec2 winSize(1040,830);
    auto frame=[&]() {
        ImGui_ImplDX11_NewFrame();ImGui::NewFrame();ImGui::PushFont(font,16.f*viewScale);
        ImGui::SetNextWindowPos(ImVec2(30,25),ImGuiCond_Always);ImGui::SetNextWindowSize(winSize,ImGuiCond_Always);
        ImGui::Begin("OptiScaler | test11 - FFXIV",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings);
        NativeMenuHints::Draw();
        if(ImGui::BeginTable("native-preview",2,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::SeparatorText(MenuI18n::Text("Upscalers"));
            const char* backends[]={"FSR 2.1 (DX11)","FSR 3.1 (DX12 bridge)"};
            ImGui::SetNextItemWidth(210*viewScale);
            MenuI18nUI::Combo("FFX Upscaler",&backend,backends,2);RecordItem("backend");
            ImGui::Button(MenuI18n::Label("Change Upscaler"));
            ImGui::SeparatorText(MenuI18n::Text("Frame Generation"));
            ImGui::Checkbox(MenuI18n::Label("Enable Frame Generation"),&fg);RecordItem("fg");
            ImGui::SeparatorText(MenuI18n::Text("Upscale Ratio Override"));
            ImGui::Checkbox(MenuI18n::Label("Override all"),&overrideRatio);
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderFloat(MenuI18n::Label("All Ratios"),&ratio,1,3,"%.2f");
            ImGui::SeparatorText(MenuI18n::Text("DRS (Dynamic Resolution Scaling)"));
            ImGui::Checkbox(MenuI18n::Label("Override Minimum"),&drsMin);
            ImGui::Checkbox(MenuI18n::Label("Override Maximum"),&drsMax);
            for(const char* label:{"Framerate","Sharpness","Advanced Settings","Logging","Menu Theme and Color","FPS Overlay","Upscaler Inputs","Keybinds"})
                ImGui::CollapsingHeader(MenuI18n::Label(label));
            ImGui::TableNextColumn();
            ImGui::SeparatorText(MenuI18n::Text("DLSS Neural Rendering"));
            ImGui::Checkbox(MenuI18n::Label("Enable Neural Rendering"),&nr);RecordItem("nr");
            ImGui::Checkbox(MenuI18n::Label("AMD: apply before Super Resolution"),&pre);RecordItem("pre");
            if(nr&&!pre)MenuI18nUI::TextWrapped("AMD NR is disabled because apply before Super Resolution is off.");
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderFloat(MenuI18n::Label("NR resolution (%)"),&scale,25,100,"%.0f%%");
            ImGui::Button(MenuI18n::Label("Reset NR resolution"));
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderInt(MenuI18n::Label("AMD neural passes"),&passes,1,3);
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderFloat(MenuI18n::Label("AMD tone (first pass)"),&tone,0,2);
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderFloat(MenuI18n::Label("AMD structure"),&structure,0,2);
            ImGui::SetNextItemWidth(160*viewScale);ImGui::SliderFloat(MenuI18n::Label("AMD skin structure"),&skin,0,2);
            MenuI18nUI::TextWrapped("AMD pre-SR: waiting for a DirectX 12 SR frame");
            MenuI18nUI::TextWrapped("AMD backend. Use one neural pass for FSR FG testing. Each pass has independent history. Restart after a backend failure.");
            ImGui::EndTable();
        }
        save=ImGui::Button(MenuI18n::Label("Save Settings"));RecordItem("save");ImGui::SameLine();
        close=ImGui::Button(MenuI18n::Label("Close"));RecordItem("close");
        if(ImGui::GetID(MenuI18n::Label("Reset##one"))==ImGui::GetID(MenuI18n::Label("Reset##two")))throw std::runtime_error("hidden ID collision");
        ImGui::End();ImGui::PopFont();ImGui::Render();
        const float clear[]={.035f,.045f,.065f,1};auto rt=rtv.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);ctx->ClearRenderTargetView(rt,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    };
    auto dump=[&](const char* path) {
        for(int i=0;i<4;i++)frame();
        D3D11_TEXTURE2D_DESC read=desc;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;Check(SUCCEEDED(dev->CreateTexture2D(&read,nullptr,&stage)),"readback texture");ctx->CopyResource(stage.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map)),"readback map");
        std::ofstream f(path,std::ios::binary);for(UINT y=0;y<height;y++)f.write(static_cast<char*>(map.pData)+y*map.RowPitch,width*4);ctx->Unmap(stage.Get(),0);
    };
    auto click=[&](const char* id) {
        frame();auto r=items.at(id);io.AddMousePosEvent(r.a.x+12,(r.a.y+r.b.y)*.5f);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
    };
    dump("native-zh.rgba");
    click("pre");Check(pre,"AMD pre-SR switch can enable");
    click("nr");Check(!nr&&pre,"NR enable is independent from placement");
    click("nr");Check(nr&&pre,"both AMD switches can stay enabled");
    click("fg");Check(fg,"FG switch commits");
    click("save");Check(save,"translated save button action");
    click("close");Check(close,"translated close button action");
    click("backend");
    frame();
    auto popup=ImGui::FindWindowByName("##Combo_00");Check(popup&&popup->Active,"backend dropdown opens");
    io.AddMousePosEvent(popup->Pos.x+25,popup->DC.CursorPosPrevLine.y+8.f);
    frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
    Check(backend==1,"backend selection preserves numeric value");
    dump("native-enabled.rgba");
    MenuI18n::enabled=false;
    Check(std::string(MenuI18n::Text("Save Settings"))=="Save Settings","English fallback");
    dump("native-en.rgba");
    MenuI18n::enabled=true;viewScale=.75f;winSize=ImVec2(810,700);dump("native-small.rgba");
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();std::cout<<"ALL UI CHECKS PASSED\n";return 0;
} catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
