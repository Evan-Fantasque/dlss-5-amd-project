#define NOMINMAX
#define SIMPLE_SETTINGS_TEST
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_dx11.h>
#include "simple_settings.h"
#include <map>
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
struct Rect { ImVec2 a,b; };
static std::map<std::string,Rect> items;
void SimpleSettings::RecordItem(const char* id) { items[id]={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()}; }
static void Check(bool ok,const char* label) { if(!ok) throw std::runtime_error(label); std::cout<<"PASS "<<label<<"\n"; }
int main(int argc, char** argv) {
try {
    const std::filesystem::path output = argc > 1 ? argv[1] : ".build/menu-preview";
    std::filesystem::create_directories(output);
    std::filesystem::current_path(output);
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx)),"WARP device");
    const UINT width=680,height=880;
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
    SimpleSettings::Model m;m.chinese=true;m.inputActive=true;m.fsrSelected=true;m.backend="FSR 4 (DX12)";m.ratioOverride=true;m.ratio=1.3f;m.fgSupported=m.fgRouteActive=m.fgRouteConfigured=m.fgEnabled=true;m.nrAvailable=m.nrEnabled=true;m.nrPercent=75.f;m.fpsLimit=60;
    SimpleSettings::EditState edits;
    float viewScale=1.f;
    ImVec2 winSize(560,840);
    auto frame=[&]() {
        ImGui_ImplDX11_NewFrame();ImGui::NewFrame();ImGui::PushFont(font,18.f*viewScale);
        ImGui::SetNextWindowPos(ImVec2(40,35),ImGuiCond_Always);ImGui::SetNextWindowSize(winSize,ImGuiCond_Always);
        ImGui::Begin(SimpleSettings::Text(m.chinese,"FFXIV AMD 简易设置###FfxivSimpleSettings","FFXIV AMD Simple Settings###FfxivSimpleSettings"),nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings);
        auto a=SimpleSettings::Draw(m,edits);ImGui::End();ImGui::PopFont();ImGui::Render();
        const float clear[]={.035f,.045f,.065f,1};auto rt=rtv.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);ctx->ClearRenderTargetView(rt,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return a;
    };
    auto dump=[&](const char* path) {
        for(int i=0;i<4;i++)frame();
        D3D11_TEXTURE2D_DESC read=desc;read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;Check(SUCCEEDED(dev->CreateTexture2D(&read,nullptr,&stage)),"readback texture");ctx->CopyResource(stage.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map)),"readback map");
        std::ofstream f(path,std::ios::binary);for(UINT y=0;y<height;y++)f.write(static_cast<char*>(map.pData)+y*map.RowPitch,width*4);ctx->Unmap(stage.Get(),0);
    };
    auto click=[&](const char* id) {
        frame();auto r=items.at(id);io.AddMousePosEvent(r.a.x+12,(r.a.y+r.b.y)*.5f);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);return frame();
    };
    Check(!SimpleSettings::FfxivReady(m), "partial FFXIV config is incomplete");
    auto preset=click("ffxivPreset");
    Check(preset.ffxivPreset && SimpleSettings::FfxivReady(m) && m.ratio==1.3f && m.upscaleRestart, "preset completes flags, preserves ratio, requests restart");
    Check(!click("ffxivPreset").ffxivPreset, "configured preset is inert");
    m.upscaleRestart=false;
    dump("simple-zh.rgba");
    m.ratio=1.5f;m.drsMin=false;m.qualityOverride=true;
    click("ratio");preset=click("ratio13");
    Check(preset.ratio && SimpleSettings::FfxivReady(m) && m.ratio==1.3f && m.upscaleRestart, "ratio selection completes FFXIV settings and requests restart");
    dump("simple-restart.rgba");
    auto a=click("nr");Check(a.nr && !m.nrEnabled,"NR off");a=click("nr");Check(a.nr && m.nrEnabled,"NR on");
    a=click("fg");Check(a.fg && !m.fgEnabled,"FG off");a=click("fg");Check(a.fg && m.fgEnabled,"FG on");
    a=click("showFps");Check(a.showFps && m.showFps,"FPS display toggle");
    a=click("full");Check(a.full,"full-settings action");a=click("save");Check(a.save,"save action");a=click("close");Check(a.close,"close action");
    auto r=items.at("nrScale");io.AddMousePosEvent(r.a.x+90,(r.a.y+r.b.y)*.5f);frame();io.AddMouseButtonEvent(0,true);a=frame();Check(!a.nrScale && m.nrPercent==75.f,"NR drag defers update");io.AddMouseButtonEvent(0,false);a=frame();Check(a.nrScale && m.nrPercent>=25 && m.nrPercent<=100,"NR release commits");
    r=items.at("tone");io.AddMousePosEvent(r.a.x+50,(r.a.y+r.b.y)*.5f);frame();io.AddMouseButtonEvent(0,true);a=frame();Check(!a.tone && m.tone==1.f,"tone drag defers update");io.AddMouseButtonEvent(0,false);a=frame();Check(a.tone && m.tone>=0 && m.tone<=2,"tone release commits");
    r=items.at("structure");io.AddMousePosEvent(r.a.x+60,(r.a.y+r.b.y)*.5f);frame();io.AddMouseButtonEvent(0,true);a=frame();Check(!a.structure && m.structure==1.f,"structure drag defers update");io.AddMouseButtonEvent(0,false);a=frame();Check(a.structure && m.structure>=0 && m.structure<=2,"structure release commits");
    m.nrAvailable=false;m.nrEnabled=false;edits={};a=click("nr");Check(!a.nr && !m.nrEnabled,"missing runtime disables NR enable");
    m.fgRouteActive=false;m.fgRouteConfigured=false;a=click("prepareFg");Check(a.prepareFg,"FG setup action");
    m.externalFg=true;frame();Check(!click("nr").nr,"disabled remains inert");m.externalFg=false;
    m.nrAvailable=m.nrEnabled=true;m.fgRouteActive=m.fgRouteConfigured=true;m.nrPercent=75;
    m.chinese=false;dump("simple-en.rgba");
    m.chinese=true;viewScale=.7f;winSize=ImVec2(400,650);dump("simple-small.rgba");
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();std::cout<<"ALL UI CHECKS PASSED\n";return 0;
} catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
