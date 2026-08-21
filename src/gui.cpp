#include "gui.hpp"
#include "GucciBot.hpp"
#include "clicksounds.hpp"
#include "autoclicker.hpp"
#include "calibration.hpp"
#include "bigbrrr.hpp"
#include "jupiterghost.hpp"
#include "trainerghost.hpp"

#include "renderer.hpp"
#include "render/renderer.hpp"
#include "selfcheck.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/Task.hpp>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <cstdint>
#include <regex>
#include <system_error>
#include <vector>
using namespace geode::prelude;

static ImVec4 lerpColor(const ImVec4& a,const ImVec4& b,float t){
    return ImVec4(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t);}
static float smoothStep(float cur,float tgt,float spd,float dt){
    return cur+(tgt-cur)*std::min(1.f,dt*spd);}
static ImVec4 withAlpha(ImVec4 c,float a){c.w=a;return c;}
static ImVec4 brighten(const ImVec4& c,float amt){
    return ImVec4(std::clamp(c.x+amt,0.f,1.f),std::clamp(c.y+amt,0.f,1.f),std::clamp(c.z+amt,0.f,1.f),c.w);}
static ImU32 toU32(const ImVec4& c){return ImGui::ColorConvertFloat4ToU32(c);}
static ImVec2 snapPos(ImVec2 p){return ImVec2(std::round(p.x),std::round(p.y));}

// Jupiter segments: shared by the Segments list itself, segment looping,
// export/import, and auto-suggestions. Raw format is "label,x,note;label,x,note;...".
// Notes are escaped (not base64 -- just swaps the two delimiter chars for a
// harmless placeholder) so a note can contain commas/semicolons without
// corrupting the field split. Old saves only have "label,x" (no note field);
// loading falls back to that when the would-be x-field doesn't parse as a
// float, so existing segments from before this feature still load correctly.
struct JupiterSegment{std::string label;float x=0.f;std::string note;};

static std::string jupEscapeField(std::string s){
    std::string out;
    for(char c:s){
        if(c==',')out+="&#44;";
        else if(c==';')out+="&#59;";
        else out+=c;
    }
    return out;
}
static std::string jupUnescapeField(std::string s){
    auto replaceAll=[](std::string& str,const std::string& from,const std::string& to){
        size_t p=0;
        while((p=str.find(from,p))!=std::string::npos){str.replace(p,from.size(),to);p+=to.size();}
    };
    replaceAll(s,"&#44;",",");
    replaceAll(s,"&#59;",";");
    return s;
}

static std::vector<JupiterSegment> parseJupiterSegments(std::string const& raw){
    std::vector<JupiterSegment> segs;
    size_t pos=0;
    while(pos<raw.size()){
        size_t semi=raw.find(';',pos);
        std::string entry=raw.substr(pos,semi==std::string::npos?std::string::npos:semi-pos);
        JupiterSegment seg;
        size_t cLast=entry.rfind(',');
        if(cLast!=std::string::npos){
            std::string beforeLast=entry.substr(0,cLast);
            std::string lastTok=entry.substr(cLast+1);
            size_t cPrev=beforeLast.rfind(',');
            bool parsedNew=false;
            if(cPrev!=std::string::npos){
                std::string xTok=beforeLast.substr(cPrev+1);
                char* endp=nullptr;
                float xv=std::strtof(xTok.c_str(),&endp);
                if(endp&&*endp=='\0'&&endp!=xTok.c_str()){
                    seg.label=beforeLast.substr(0,cPrev);
                    seg.x=xv;
                    seg.note=jupUnescapeField(lastTok);
                    parsedNew=true;
                }
            }
            if(!parsedNew){
                seg.label=beforeLast;
                try{seg.x=std::stof(lastTok);}catch(...){}
            }
            segs.push_back(seg);
        }
        if(semi==std::string::npos)break;
        pos=semi+1;
    }
    return segs;
}

static std::string serializeJupiterSegments(std::vector<JupiterSegment> const& segs){
    std::string out;
    for(size_t i=0;i<segs.size();i++){
        if(i)out+=";";
        out+=segs[i].label+","+std::to_string(segs[i].x)+","+jupEscapeField(segs[i].note);
    }
    return out;
}

// Minimal base64 codec -- just enough to turn the segments+notes blob into a
// single opaque, clipboard-safe string for export/import. Standard 6-bit
// accumulator implementation, nothing GD-specific about it.
static const char kB64Chars[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64Encode(std::string const& in){
    std::string out;
    int val=0,valb=-6;
    for(unsigned char c:in){
        val=(val<<8)+c;
        valb+=8;
        while(valb>=0){out.push_back(kB64Chars[(val>>valb)&0x3F]);valb-=6;}
    }
    if(valb>-6)out.push_back(kB64Chars[((val<<8)>>(valb+8))&0x3F]);
    while(out.size()%4)out.push_back('=');
    return out;
}
static std::string base64Decode(std::string const& in){
    int T[256];std::fill(std::begin(T),std::end(T),-1);
    for(int i=0;i<64;i++)T[(unsigned char)kB64Chars[i]]=i;
    std::string out;
    int val=0,valb=-8;
    for(unsigned char c:in){
        if(T[c]==-1)continue;
        val=(val<<6)+T[c];
        valb+=6;
        if(valb>=0){out.push_back((char)((val>>valb)&0xFF));valb-=8;}
    }
    return out;
}

// Export/import: bundles segments + notes into one opaque code, using an
// ASCII Unit Separator (0x1F) between the two blobs since it'll never
// legitimately appear in either (segments already escape their own
// delimiters, notes are free text but 0x1F isn't typeable in the InputText).
static std::string exportSegmentsCode(std::string const& segmentsRaw,std::string const& notes){
    std::string blob=segmentsRaw+"\x1F"+notes;
    return "JMF1:"+base64Encode(blob);
}
static bool importSegmentsCode(std::string const& code,std::string& outSegmentsRaw,std::string& outNotes,std::string& err){
    static const std::string kPrefix="JMF1:";
    if(code.compare(0,kPrefix.size(),kPrefix)!=0){err="Not a valid segment code.";return false;}
    std::string blob=base64Decode(code.substr(kPrefix.size()));
    size_t sep=blob.find('\x1F');
    if(sep==std::string::npos){err="Corrupted code.";return false;}
    outSegmentsRaw=blob.substr(0,sep);
    outNotes=blob.substr(sep+1);
    return true;
}

// Auto segment suggestions: buckets click PRESS times (GucciReplaySystem::
// m_clickIntervalsSec, already built at load()) into 0.5s windows, flags
// windows with unusually high click density, and looks up the player's
// actual X position at that moment via m_pathSamples (index==frame) to turn
// "a lot of clicks happened around here in time" into "here's roughly where
// that was in the level". Skips anything within 50 units of an existing
// segment so repeated presses don't spam duplicates.
static std::vector<JupiterSegment> suggestSegmentsFromClickDensity(
    std::vector<std::pair<double,double>> const& clickIntervalsSec,
    std::vector<MacroPathSample> const& pathSamples,
    double clickBarTps,
    std::string const& existingSegmentsRaw){
    std::vector<JupiterSegment> out;
    if(clickIntervalsSec.empty()||pathSamples.empty())return out;
    double tps=clickBarTps>0.0?clickBarTps:240.0;

    double maxT=0.0;
    for(auto const& iv:clickIntervalsSec)maxT=std::max(maxT,iv.second);
    if(maxT<=0.0)return out;

    const double bucketSec=0.5;
    int nBuckets=(int)(maxT/bucketSec)+1;
    std::vector<int> counts(nBuckets,0);
    for(auto const& iv:clickIntervalsSec){
        int b=std::clamp((int)(iv.first/bucketSec),0,nBuckets-1);
        counts[b]++;
    }
    double meanCount=0.0;
    for(int c:counts)meanCount+=c;
    meanCount/=std::max(1,nBuckets);

    struct Cand{int bucket;int count;};
    std::vector<Cand> cands;
    for(int b=0;b<nBuckets;b++)
        if(counts[b]>=3&&(double)counts[b]>meanCount*2.0)cands.push_back({b,counts[b]});
    std::sort(cands.begin(),cands.end(),[](Cand const&a,Cand const&b){return a.count>b.count;});
    if(cands.size()>5)cands.resize(5);

    auto existing=parseJupiterSegments(existingSegmentsRaw);
    for(auto const& c:cands){
        double midSec=(c.bucket+0.5)*bucketSec;
        uint32_t frame=(uint32_t)(midSec*tps);
        if(frame>=pathSamples.size())continue;
        float x=pathSamples[frame].p1x;
        bool dup=false;
        for(auto const& s:existing)if(std::fabs(s.x-x)<50.f){dup=true;break;}
        for(auto const& s:out)if(std::fabs(s.x-x)<50.f){dup=true;break;}
        if(dup)continue;
        JupiterSegment seg;
        seg.label="Auto: dense clicks ("+std::to_string(c.count)+"/500ms)";
        seg.x=x;
        out.push_back(seg);
    }
    return out;
}

// BIG BRRRR bounce, take 2. The first version nudged the window by the DELTA
// between this frame's and last frame's sine value -- a RELATIVE correction
// that silently assumes nothing else ever touches the window's position
// between calls. That assumption doesn't actually hold: this window has no
// NoMove flag, so ImGui's own native click-drag can reposition it too, and a
// relative delta has no way to detect or correct for that -- it just keeps
// nudging from wherever the window happens to be, which can drift away from
// true rest and never visibly "come back down." This version tracks an
// explicit ABSOLUTE anchor (restY, captured the moment bouncing starts) and
// always drives the window to restY+offset directly, rather than trusting
// GetWindowPos() to reflect only what this function itself did last frame.
// Once offset decays to 0 it goes fully idle and stops touching Y at all, so
// the window ends up exactly back at its real rest position, and normal
// dragging works unaffected while idle (same as before BRRRR existed).
// Shared between drawMainWindow and drawMegaHackWindow so both skins bounce
// in sync; never applies to the Jupiter tab (jupiterActive guard) -- and
// that resets everything too, since its position is force-set every frame
// anyway, so BRRRR re-anchors cleanly if it's still on when you leave.
static void applyBigBrrrBounce(bool jupiterActive){
    static float restY=0.f;
    static float offset=0.f;
    static bool active=false;
    // ImGui::GetTime() value corresponding to the track's position 0 (an
    // assumed downbeat) -- captured fresh each time bouncing (re)starts.
    // Playback itself starts kStartOffsetSec into the file (skips the slow
    // intro, see BigBrrrManager::start), so that moment is kStartOffsetSec
    // seconds AFTER this reference point, not at it.
    static double beatRefTime=0.0;

    if(jupiterActive){active=false;offset=0.f;return;}

    bool on=BigBrrrManager::get()->enabled;
    if(on){
        if(!active){
            restY=ImGui::GetWindowPos().y-offset;
            active=true;
            beatRefTime=ImGui::GetTime()-BigBrrrManager::kStartOffsetSec;
        }
        double elapsed=ImGui::GetTime()-beatRefTime;
        double omega=2.0*3.14159265358979*BigBrrrManager::kBpm/60.0; // one full bounce cycle per beat
        offset=(float)(std::sin(elapsed*omega)*10.0);
    } else if(active){
        offset*=0.75f;
        if(std::fabs(offset)<0.05f){offset=0.f;active=false;}
    } else {
        return;
    }

    ImVec2 wp=ImGui::GetWindowPos();
    ImGui::SetWindowPos(ImVec2(wp.x,restY+offset));
}

static const char* getAccuracyTag(AccuracyMode m){
    switch(m){case AccuracyMode::CBS:return "CBS";case AccuracyMode::CBF:return "CBF";default:return nullptr;}}
static ImVec4 getAccuracyTagColor(AccuracyMode m){
    switch(m){case AccuracyMode::CBS:case AccuracyMode::CBF:return ImVec4(1.f,0.22f,0.22f,1.f);default:return ImVec4(1,1,1,1);}}
static ImVec4 getBRRTagColor(){return ImVec4(0.30f,0.70f,1.0f,1.0f);}

static float sanitizeClamped(float v,float lo,float hi,float fb){
    if(!std::isfinite(v))return fb;return std::clamp(v,lo,hi);}
static ImVec4 sanitizeColor(ImVec4 v,ImVec4 fb){
    if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||!std::isfinite(v.w))return fb;
    float mx=std::max({v.x,v.y,v.z,v.w});
    if(mx>1.0001f&&mx<=255.f){v.x/=255.f;v.y/=255.f;v.z/=255.f;v.w/=255.f;}
    v.x=std::clamp(v.x,0.f,1.f);v.y=std::clamp(v.y,0.f,1.f);
    v.z=std::clamp(v.z,0.f,1.f);v.w=std::clamp(v.w,0.f,1.f);return v;}

template<class T>
static T loadSV(Mod* mod,std::string_view key,T def,
    std::initializer_list<std::string_view> legacy={}){
    if(mod->hasSavedValue(std::string(key)))return mod->getSavedValue<T>(std::string(key),def);
    for(auto lk:legacy)if(mod->hasSavedValue(std::string(lk)))return mod->getSavedValue<T>(std::string(lk),def);
    return def;}

static void drawSolidRect(ImDrawList* dl,ImVec2 mn,ImVec2 mx,float r,const ThemeEngine& t,float a,bool border=true){
    ImVec4 fill(t.cardColor.x,t.cardColor.y,t.cardColor.z,t.cardColor.w*a);
    dl->AddRectFilled(mn,mx,toU32(fill),r);
    if(border)dl->AddRect(mn,mx,t.getAccentU32(0.18f*a),r,0,1.f);}

// N-pointed outline star, points alternating outer/inner radius, as a closed polyline.
static std::vector<ImVec2> jupiterStarPoints(ImVec2 center,float outerR,float innerR,int points,float rotRad){
    std::vector<ImVec2> pts;
    int total=points*2;
    for(int i=0;i<total;i++){
        float r=(i%2==0)?outerR:innerR;
        float a=rotRad+(float)i/(float)total*2.0f*3.14159265f;
        pts.push_back(ImVec2(center.x+r*cosf(a),center.y+r*sinf(a)));
    }
    return pts;
}


// The whole menu window becomes the art piece when the Jupiter tab is active --
// not a themed box living inside a normal-looking app. Pulled from Nigel's 8
// checkpoint screenshots: the gold orbit-ring-with-star ornament, scattered
// outline stars, a faint crosshatch grid, drifting cyan pixel-squares, and a
// dark skyline silhouette. Hand-drawn vector shapes (no ripped assets), drawn
// on the WINDOW draw list across the full window rect (title bar, tab rail,
// content, status bar) so nothing about the window reads as "normal app skin
// plus a themed tab" -- everything behind the widgets is reskinned.
// "Wheel Thing", take 4 -- Nigel sent a real screenshot of the actual level
// this time. It's a half-dome sitting on the ground (not a full circle):
// ring ARCS (top half only) with dots sitting ON each ring at intervals
// (not filling gaps between rings), and a solid filled core at the bottom
// center with bold triangular spike rays fanning up through the dome.
// `center` is the BOTTOM-CENTER anchor (the dome's flat edge), not the
// middle of a full circle.
static void drawJupiterOrnament(ImDrawList* dl,ImVec2 center,float baseR,float time,float spin){
    const ImU32 gold=IM_COL32(252,245,80,255);
    const float PI=3.14159265f;

    const int nRings=4;
    float ringR[nRings];
    for(int i=0;i<nRings;i++){
        ringR[i]=baseR*(0.34f+0.22f*(float)i);
        const int segs=48;
        std::vector<ImVec2> arc(segs+1);
        for(int s=0;s<=segs;s++){
            float a=PI+(float)s/(float)segs*PI; // sweeps the TOP half only
            arc[s]=ImVec2(center.x+ringR[i]*cosf(a),center.y+ringR[i]*sinf(a));
        }
        dl->AddPolyline(arc.data(),segs+1,gold,0,3.2f);
    }

    // dots sitting ON each ring at intervals, like the reference's sundial
    // markings, plus a couple of short radial tick marks per ring.
    for(int i=0;i<nRings;i++){
        int count=6+i*3;
        for(int d=1;d<count;d++){
            float a=PI+(float)d/(float)count*PI;
            ImVec2 p(center.x+ringR[i]*cosf(a),center.y+ringR[i]*sinf(a));
            if(d%2==1)dl->AddCircleFilled(p,5.f,gold,12);
            else{
                ImVec2 t0(center.x+(ringR[i]-6.f)*cosf(a),center.y+(ringR[i]-6.f)*sinf(a));
                ImVec2 t1(center.x+(ringR[i]+6.f)*cosf(a),center.y+(ringR[i]+6.f)*sinf(a));
                dl->AddLine(t0,t1,gold,2.4f);
            }
        }
    }

    // solid filled core + bold triangular spike rays fanning up from it,
    // reaching roughly to the innermost ring -- matches the reference's
    // sunburst core, not thin lines.
    float coreR=baseR*0.14f;
    int nRays=11;
    for(int i=0;i<=nRays;i++){
        float a=PI+(float)i/(float)nRays*PI+time*spin*0.15f;
        float rayLen=ringR[0]*1.05f;
        float halfW=coreR*0.32f;
        float pa=a+PI*0.5f;
        ImVec2 tip(center.x+rayLen*cosf(a),center.y+rayLen*sinf(a));
        ImVec2 base1(center.x+halfW*cosf(pa),center.y+halfW*sinf(pa));
        ImVec2 base2(center.x-halfW*cosf(pa),center.y-halfW*sinf(pa));
        dl->AddTriangleFilled(base1,base2,tip,gold);
    }
    dl->AddCircleFilled(center,coreR,gold,48);
}

// Wave ribbon, take 12: same \/\  zigzag + shifted-copy + parallel-connector
// construction as last round (that part worked), shifted further right so
// it clears the real tab content instead of clipping into it.
static void drawJupiterWaveRibbon(ImDrawList* dl,ImVec2 pos,ImVec2 size){
    const ImU32 gold=IM_COL32(252,245,80,255);

    struct Pt{float x,y;};
    static const Pt spine[]={
        {0.38f,0.00f},{0.50f,0.33f},{0.36f,0.66f},{0.48f,1.00f},
    };
    const int n=(int)(sizeof(spine)/sizeof(spine[0]));
    const float offsetX=size.x*0.08f;

    std::vector<ImVec2> orig(n),copy(n);
    for(int i=0;i<n;i++){
        orig[i]=ImVec2(pos.x+size.x*spine[i].x,pos.y+size.y*spine[i].y);
        copy[i]=ImVec2(orig[i].x+offsetX,orig[i].y);
    }
    dl->AddPolyline(orig.data(),n,gold,0,6.f);
    dl->AddPolyline(copy.data(),n,gold,0,6.f);

    std::vector<float> segLen(n-1);
    float totalLen=0.f;
    for(int i=0;i<n-1;i++){
        float dx=orig[i+1].x-orig[i].x,dy=orig[i+1].y-orig[i].y;
        segLen[i]=sqrtf(dx*dx+dy*dy);
        totalLen+=segLen[i];
    }

    const float step=13.f;
    int seg=0; float segPos=0.f;
    for(float dist=0.f;dist<totalLen;dist+=step,segPos+=step){
        while(seg<n-2&&segPos>segLen[seg]){segPos-=segLen[seg];seg++;}
        float segL=segLen[seg]>0.001f?segLen[seg]:0.001f;
        float t=segPos/segL;
        ImVec2 a=orig[seg],b=orig[seg+1];
        ImVec2 pO(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t);
        ImVec2 pC(pO.x+offsetX,pO.y);
        dl->AddLine(pO,pC,gold,2.6f);
    }
}

static void drawJupiterBackdrop(ImDrawList* dl,ImVec2 pos,ImVec2 size,float time){
    drawJupiterWaveRibbon(dl,pos,size);

    // "Wheel Thing" -- half-dome sun, tucked into the bottom-right corner.
    // Fixed pixel radius (not a fraction of the viewport width) so it stays
    // small and proportionate to the stars instead of ballooning into
    // everything else on wide windows -- that was the overlap bug.
    drawJupiterOrnament(dl,ImVec2(pos.x+size.x*0.93f,pos.y+size.y),76.f,time,0.05f);

    // star cluster, upper-right -- 5-pointed only, each with a pentagon
    // outline nested in the middle built directly from the star's own inner
    // (concave) vertices -- jupiterStarPoints alternates outer/inner points,
    // so the odd indices ARE the inner ring already, guaranteeing alignment
    // instead of recomputing angles separately (which was off by half a step).
    struct StarSpec{float x,y,r,rot;};
    static const StarSpec stars[]={
        {0.65f,0.22f,58.f,0.3f},{0.85f,0.44f,52.f,1.1f},{0.66f,0.60f,50.f,0.7f},
        {0.76f,0.33f,26.f,2.0f},{0.92f,0.58f,22.f,1.4f},{0.57f,0.70f,24.f,0.4f},
        {0.79f,0.68f,28.f,2.6f},{0.89f,0.16f,24.f,0.9f},{0.60f,0.40f,20.f,1.6f},
    };
    for(auto const& s:stars){
        ImVec2 c(pos.x+size.x*s.x,pos.y+size.y*s.y);
        auto pts=jupiterStarPoints(c,s.r,s.r*0.42f,5,s.rot);
        dl->AddPolyline(pts.data(),(int)pts.size(),IM_COL32(252,245,80,255),ImDrawFlags_Closed,3.5f);
        std::vector<ImVec2> pent;
        for(int k=1;k<(int)pts.size();k+=2)pent.push_back(pts[k]);
        dl->AddPolyline(pent.data(),(int)pent.size(),IM_COL32(252,245,80,255),ImDrawFlags_Closed,2.2f);
    }
}

namespace{
static std::filesystem::path getReplayDir(){return GucciEngine::get()->getReplayDir();}
static bool renameStoredReplay(const std::string& oldN,const std::string& req,
    std::string& finalN,std::string& err){
    err.clear();
    auto sanitized=req;
    if(sanitized.empty()){err="Name cannot be empty.";return false;}
    auto dir=getReplayDir();
    std::error_code ec;
        std::filesystem::path oldPath;
    for(auto& e:std::filesystem::directory_iterator(dir,ec)){
        if(e.is_regular_file()&&e.path().stem().string()==oldN){oldPath=e.path();break;}}
    if(oldPath.empty()){err="Original file not found.";return false;}
    auto newName=sanitized;
    auto newPath=dir/(newName+oldPath.extension().string());
    std::filesystem::rename(oldPath,newPath,ec);
    if(ec){err="Rename failed: "+ec.message();return false;}
    finalN=newName;return true;}

static bool deleteStoredReplay(const std::string& name,std::string& err){
    err.clear();
    auto dir=getReplayDir();
    std::error_code ec;
        std::filesystem::path found;
    for(auto& e:std::filesystem::directory_iterator(dir,ec)){
        if(e.is_regular_file()&&e.path().stem().string()==name){found=e.path();break;}}
    if(found.empty()){err="File not found.";return false;}
    std::filesystem::remove(found,ec);
    if(ec){err="Delete failed: "+ec.message();return false;}
    return true;}

static void drawPopupChrome(MenuInterface& ui,const char* title,float rounding=0.f,float titleBandH=28.f){
    ImVec2 wp=snapPos(ImGui::GetWindowPos()),ws=snapPos(ImGui::GetWindowSize());
    ImDrawList* dl=ImGui::GetWindowDrawList(),*fg=ImGui::GetForegroundDrawList();
    ImVec2 wm(wp.x+ws.x,wp.y+ws.y);
    drawSolidRect(dl,wp,wm,rounding,ui.theme,0.72f,false);
    fg->AddRect(wp,wm,ui.theme.getAccentU32(0.36f),rounding,0,1.f);
    float ty=wp.y+12.f;
    dl->AddText(ImVec2(wp.x+14.f,ty),ui.theme.getTextU32(),title);
    float dy=ty+ImGui::GetFontSize()+10.f;
    dl->AddLine(ImVec2(wp.x+1.f,dy),ImVec2(wp.x+ws.x-1.f,dy),ui.theme.getAccentU32(0.30f),1.f);
    ImGui::Dummy(ImVec2(0.f,titleBandH+8.f));}}

namespace Widgets{
void GucciQuote(const char* quote,const char* attr,ThemeEngine& theme){
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(pos,ImVec2(pos.x+3,pos.y+36),theme.getAccentU32(0.7f),1.f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x+10,pos.y+2));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
    ImGui::TextUnformatted(quote);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(pos.x+10,pos.y+18));
    ImGui::PushStyleColor(ImGuiCol_Text,withAlpha(theme.getAccent(),0.6f));
    ImGui::TextUnformatted(attr);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(pos.x,pos.y+40));
    ImGui::Dummy(ImVec2(0,0));
}}

static const ThemePreset kThemePresets[]={
        {"GucciBot",
     ImVec4(0.788f,0.659f,0.298f,1.f),
     ImVec4(0.051f,0.051f,0.051f,0.96f),
     ImVec4(0.078f,0.078f,0.078f,1.f),
     ImVec4(0.941f,0.910f,0.816f,1.f),
     ImVec4(0.478f,0.447f,0.376f,1.f),
     5.f,0.96f},
        {"ToosiiBot (LSU)",
     ImVec4(0.992f,0.816f,0.137f,1.f),
     ImVec4(0.110f,0.055f,0.188f,0.96f),
     ImVec4(0.165f,0.082f,0.275f,1.f),
     ImVec4(0.960f,0.940f,0.870f,1.f),
     ImVec4(0.600f,0.490f,0.300f,1.f),
     5.f,0.96f},
        {"ToosiiBot (Syracuse)",
     ImVec4(0.961f,0.404f,0.031f,1.f),
     ImVec4(0.027f,0.043f,0.114f,0.96f),
     ImVec4(0.055f,0.082f,0.188f,1.f),
     ImVec4(0.960f,0.940f,0.920f,1.f),
     ImVec4(0.600f,0.500f,0.400f,1.f),
     5.f,0.96f},
        {"ToosiiBot (Sac State)",
     ImVec4(0.918f,0.878f,0.820f,1.f),
     ImVec4(0.016f,0.188f,0.094f,0.96f),
     ImVec4(0.024f,0.251f,0.125f,1.f),
     ImVec4(0.940f,0.960f,0.940f,1.f),
     ImVec4(0.500f,0.650f,0.520f,1.f),
     5.f,0.96f},
        {"JaBot",
     ImVec4(0.420f,0.784f,0.953f,1.f),
     ImVec4(0.027f,0.078f,0.200f,0.96f),
     ImVec4(0.047f,0.118f,0.275f,1.f),
     ImVec4(0.920f,0.950f,0.980f,1.f),
     ImVec4(0.400f,0.540f,0.720f,1.f),
     5.f,0.96f},
        {"GiddeyBot",
     ImVec4(0.871f,0.122f,0.122f,1.f),
     ImVec4(0.098f,0.039f,0.039f,0.96f),
     ImVec4(0.157f,0.063f,0.063f,1.f),
     ImVec4(0.980f,0.960f,0.960f,1.f),
     ImVec4(0.620f,0.420f,0.420f,1.f),
     5.f,0.96f},
        {"BamBot",
     ImVec4(0.878f,0.067f,0.153f,1.f),
     ImVec4(0.047f,0.027f,0.071f,0.96f),
     ImVec4(0.078f,0.043f,0.114f,1.f),
     ImVec4(0.980f,0.980f,0.980f,1.f),
     ImVec4(0.600f,0.400f,0.460f,1.f),
     5.f,0.97f},
        {"SexyyBot",
     ImVec4(0.910f,0.004f,0.580f,1.f),
     ImVec4(0.063f,0.016f,0.094f,0.97f),
     ImVec4(0.102f,0.027f,0.149f,1.f),
     ImVec4(0.990f,0.950f,0.980f,1.f),
     ImVec4(0.580f,0.340f,0.520f,1.f),
     5.f,0.97f},
        {"JuiceBot",
     ImVec4(0.960f,0.520f,0.380f,1.f),
     ImVec4(0.020f,0.090f,0.086f,0.96f),
     ImVec4(0.035f,0.130f,0.122f,1.f),
     ImVec4(0.980f,0.960f,0.940f,1.f),
     ImVec4(0.625f,0.565f,0.478f,1.f),
     5.f,0.96f},
        {"ButlerBot",
     ImVec4(0.996f,0.725f,0.153f,1.f),
     ImVec4(0.020f,0.050f,0.130f,0.96f),
     ImVec4(0.035f,0.085f,0.200f,1.f),
     ImVec4(0.980f,0.970f,0.940f,1.f),
     ImVec4(0.560f,0.520f,0.380f,1.f),
     5.f,0.96f},
};

ImVec4 ThemeEngine::getAccent() const{
    if(glowCycleEnabled)return computeCycleColor(glowCycleRate);
    return accentColor;}
ImVec4 ThemeEngine::getGlowAccent() const{return accentColor;}
ImVec4 ThemeEngine::computeCycleColor(float rate) const{
    float t=(float)ImGui::GetTime()*rate;
    float r=0.5f+0.5f*std::sin(t);
    float g=0.5f+0.5f*std::sin(t+2.094f);
    float b=0.5f+0.5f*std::sin(t+4.189f);
    return ImVec4(r,g,b,1.f);}
ImU32 ThemeEngine::getAccentU32(float a) const{ImVec4 c=getAccent();c.w=a;return toU32(c);}
ImU32 ThemeEngine::getAccentDimU32(float f) const{
    ImVec4 c=getAccent();c.x*=f;c.y*=f;c.z*=f;return toU32(c);}
ImU32 ThemeEngine::getTextU32() const{return toU32(textPrimary);}
ImU32 ThemeEngine::getTextSecondaryU32() const{return toU32(textSecondary);}
ImU32 ThemeEngine::getCardU32() const{return toU32(cardColor);}
void ThemeEngine::applyToImGuiStyle(){
    ImGuiStyle& s=ImGui::GetStyle();
    ImVec4 accent=getAccent();
    s.WindowRounding=s.FrameRounding=s.PopupRounding=s.ScrollbarRounding=cornerRadius;
    s.WindowPadding=ImVec2(14,12);
    ImVec4* col=s.Colors;
    col[ImGuiCol_WindowBg]=withAlpha(bgColor,bgOpacity);
    col[ImGuiCol_ChildBg]=ImVec4(0,0,0,0);
    col[ImGuiCol_PopupBg]=withAlpha(bgColor,0.94f);
    col[ImGuiCol_Border]=withAlpha(accent,0.22f);
    col[ImGuiCol_FrameBg]=withAlpha(cardColor,0.6f);
    col[ImGuiCol_FrameBgHovered]=withAlpha(cardColor,0.8f);
    col[ImGuiCol_FrameBgActive]=withAlpha(cardColor,1.f);
    col[ImGuiCol_TitleBg]=col[ImGuiCol_TitleBgActive]=col[ImGuiCol_TitleBgCollapsed]=withAlpha(bgColor,1.f);
    col[ImGuiCol_ScrollbarBg]=ImVec4(0,0,0,0);
    col[ImGuiCol_ScrollbarGrab]=withAlpha(accent,0.35f);
    col[ImGuiCol_ScrollbarGrabHovered]=withAlpha(accent,0.55f);
    col[ImGuiCol_ScrollbarGrabActive]=withAlpha(accent,0.75f);
    col[ImGuiCol_SliderGrab]=accent;
    col[ImGuiCol_SliderGrabActive]=brighten(accent,0.15f);
    col[ImGuiCol_Button]=withAlpha(cardColor,0.7f);
    col[ImGuiCol_ButtonHovered]=withAlpha(accent,0.22f);
    col[ImGuiCol_ButtonActive]=withAlpha(accent,0.38f);
    col[ImGuiCol_Header]=withAlpha(accent,0.18f);
    col[ImGuiCol_HeaderHovered]=withAlpha(accent,0.28f);
    col[ImGuiCol_HeaderActive]=withAlpha(accent,0.38f);
    col[ImGuiCol_CheckMark]=accent;
    col[ImGuiCol_Text]=textPrimary;
    col[ImGuiCol_TextDisabled]=textSecondary;
    col[ImGuiCol_Separator]=withAlpha(accent,0.18f);}
void ThemeEngine::resetDefaults(){
    accentColor=ImVec4(0.788f,0.659f,0.298f,1.f);
    bgColor=ImVec4(0.051f,0.051f,0.051f,0.96f);
    cardColor=ImVec4(0.078f,0.078f,0.078f,1.f);
    textPrimary=ImVec4(0.941f,0.910f,0.816f,1.f);
    textSecondary=ImVec4(0.478f,0.447f,0.376f,1.f);
    bgOpacity=0.96f;cornerRadius=5.f;textScale=1.f;
    glowCycleEnabled=false;glowCycleRate=0.5f;activePreset=0;}
void ThemeEngine::applyPreset(int i){
    if(i<0||i>=getPresetCount())return;
    const auto& p=kThemePresets[i];
    accentColor=p.accent;bgColor=p.bg;cardColor=p.card;
    textPrimary=p.textPrimary;textSecondary=p.textSecondary;
    cornerRadius=p.cornerRadius;bgOpacity=p.bgOpacity;activePreset=i;}
const ThemePreset* ThemeEngine::getPresets(){return kThemePresets;}
int ThemeEngine::getPresetCount(){return sizeof(kThemePresets)/sizeof(kThemePresets[0]);}

float AnimationState::easeOutCubic(float t){t=std::clamp(t,0.f,1.f);float i=1.f-t;return 1.f-i*i*i;}
float AnimationState::easeInOutQuad(float t){t=std::clamp(t,0.f,1.f);return t<0.5f?2*t*t:1.f-((-2*t+2)*(-2*t+2))/2.f;}
void AnimationState::update(float dt){
    if(dt>0.05f)dt=0.05f;
    float step=dt*animSpeed;
    if(opening){openProgress+=step;if(openProgress>=1.f){openProgress=1.f;opening=false;}}
    if(closing){openProgress-=step;if(openProgress<=0.f){openProgress=0.f;closing=false;}}
    tabTransition=std::min(1.f,tabTransition+dt*animSpeed*1.2f);}

MenuInterface* MenuInterface::get(){static MenuInterface* s=new MenuInterface();return s;}

void MenuInterface::markReplayListDirty(bool queueRefresh){
    replayListDirty=true;if(queueRefresh)replayRefreshQueued=true;}
bool MenuInterface::hasReplayDirectoryChanged() const{
    std::error_code ec;
    auto dir=getReplayDir();
    if(!std::filesystem::exists(dir,ec)||ec){return!replayDirTimeValid;}
    auto t=std::filesystem::last_write_time(dir,ec);
    if(ec)return false;
    if(!replayDirTimeValid)return true;
    return t!=replayDirLastWriteTime;}
void MenuInterface::captureReplayDirectoryTimestamp(){
    std::error_code ec;auto dir=getReplayDir();
    if(!std::filesystem::exists(dir,ec)||ec){replayDirTimeValid=false;return;}
    replayDirLastWriteTime=std::filesystem::last_write_time(dir,ec);
    replayDirTimeValid=!ec;}
void MenuInterface::refreshReplayListIfNeeded(bool force){
    if(!force&&!replayRefreshQueued&&!hasReplayDirectoryChanged())return;
    GucciEngine::get()->reloadMacroList();
    captureReplayDirectoryTimestamp();
    replayListDirty=false;replayRefreshQueued=false;}

std::string getKeyName(int code){
    if(code==0)return "None";
    if(code==9)return "Tab";if(code==13)return "Enter";if(code==27)return "Escape";
    if(code==32)return "Space";if(code==8)return "Backspace";if(code==46)return "Delete";
    if(code==0xA4)return "L.Alt";if(code==0xA5)return "R.Alt";if(code==0x12)return "Alt";
    if(code>=65&&code<=90){char s[2]={(char)code,0};return s;}
    if(code>=48&&code<=57){char s[2]={(char)code,0};return s;}
    if(code>=112&&code<=123){char s[4];snprintf(s,sizeof(s),"F%d",code-111);return s;}
    char s[32];snprintf(s,sizeof(s),"0x%X",code);return s;}

namespace Widgets{
bool ToggleSwitch(const char* label,bool* value,ThemeEngine& theme,AnimationState& anim){
    ImGuiID id=ImGui::GetID(label);
    float& t=anim.toggleAnims[id];
    float target=*value?1.f:0.f;
    t=t+(target-t)*std::min(1.f,ImGui::GetIO().DeltaTime*14.f);
    t=std::clamp(t,0.f,1.f);
    ImVec2 cursor=ImGui::GetCursorScreenPos();
                    const float trackW=36.f,trackH=16.f,knobR=6.f;
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 trackMin=cursor,trackMax(cursor.x+trackW,cursor.y+trackH);
    ImGui::InvisibleButton(label,ImVec2(ImGui::GetContentRegionAvail().x,trackH));
    bool clicked=ImGui::IsItemClicked();
    if(clicked)*value=!*value;
    bool hovered=ImGui::IsItemHovered();
    ImVec4 trackCol=lerpColor(ImVec4(0.22f,0.22f,0.22f,1.f),withAlpha(theme.getAccent(),0.75f),t);
    dl->AddRectFilled(trackMin,trackMax,toU32(trackCol),trackH*0.5f);
    float capR=trackH*0.5f;
    float knobCX=trackMin.x+capR+(trackW-2.f*capR)*t;
    float knobCY=trackMin.y+capR;
    ImVec4 knobCol=lerpColor(ImVec4(0.55f,0.55f,0.55f,1.f),ImVec4(1.f,1.f,1.f,1.f),t);
    dl->AddCircleFilled(ImVec2(knobCX,knobCY),knobR,toU32(knobCol));
    ImVec2 textPos(trackMin.x+trackW+8.f,trackMin.y+(trackH-ImGui::GetTextLineHeight())*0.5f);
    dl->AddText(textPos,hovered?theme.getAccentU32():theme.getTextU32(),label);
    return clicked;}

bool StyledButton(const char* label,ImVec2 size,ThemeEngine& theme,AnimationState& anim,float roundingOverride){
    float r=roundingOverride>=0?roundingOverride:theme.cornerRadius;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,r);
    ImGui::PushStyleColor(ImGuiCol_Button,withAlpha(theme.cardColor,0.7f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,withAlpha(theme.getAccent(),0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,withAlpha(theme.getAccent(),0.38f));
    bool r2=ImGui::Button(label,size);
    ImGui::PopStyleColor(3);ImGui::PopStyleVar();return r2;}

bool StyledSliderFloat(const char* label,float* v,float lo,float hi,ThemeEngine& theme,bool allowInput){
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,theme.getAccent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,brighten(theme.getAccent(),0.15f));
    bool changed;
    if(allowInput){
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-80);
        changed=ImGui::SliderFloat(label,v,lo,hi,"%.2f");
        ImGui::SameLine(0,6);
        char buf[16];snprintf(buf,sizeof(buf),"%.2f",*v);
        ImGui::SetNextItemWidth(70);
        char ib[16];snprintf(ib,sizeof(ib),"##%s_in",label);
        if(ImGui::InputFloat(ib,v,0,0,"%.2f"))changed=true;
    }else{
        ImGui::SetNextItemWidth(-1);
        changed=ImGui::SliderFloat(label,v,lo,hi,"%.2f");}
    ImGui::PopStyleColor(2);return changed;}

bool StyledSliderInt(const char* label,int* v,int lo,int hi,ThemeEngine& theme){
                ImGui::TextUnformatted(label);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,theme.getAccent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,brighten(theme.getAccent(),0.15f));
    ImGui::SetNextItemWidth(-1);
    char hidden[160]; snprintf(hidden,sizeof(hidden),"##%s",label);
    bool r=ImGui::SliderInt(hidden,v,lo,hi);
    ImGui::PopStyleColor(2);return r;}

void SectionHeader(const char* text,ThemeEngine& theme){
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 p=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x;
    ImVec2 sz=ImGui::CalcTextSize(text);
    dl->AddText(p,theme.getAccentU32(),text);
    float lineY=p.y+sz.y*0.5f;
    float lineX=p.x+sz.x+8.f;
    dl->AddLine(ImVec2(lineX,lineY),ImVec2(p.x+w,lineY),theme.getAccentU32(0.25f),1.f);
    ImGui::Dummy(ImVec2(0,sz.y+4.f));}

bool ModuleCard(const char* name,const char* desc,bool* enabled,
    ThemeEngine& theme,AnimationState& anim,int* keybind){
    ImGuiID id=ImGui::GetID(name);
    float& hov=anim.hoverAnims[id];
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x,h=46.f;
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(name,ImVec2(w,h));
    bool clicked=ImGui::IsItemClicked();
    bool isHov=ImGui::IsItemHovered();
    hov=smoothStep(hov,isHov?1.f:0.f,12.f,ImGui::GetIO().DeltaTime);
    ImVec4 bg=lerpColor(theme.cardColor,brighten(theme.cardColor,0.05f),hov);
    dl->AddRectFilled(pos,ImVec2(pos.x+w,pos.y+h),toU32(bg),theme.cornerRadius);
    dl->AddRect(pos,ImVec2(pos.x+w,pos.y+h),theme.getAccentU32(*enabled?0.45f:0.12f),theme.cornerRadius,0,*enabled?1.2f:0.5f);
    if(*enabled)dl->AddRectFilled(pos,ImVec2(pos.x+3,pos.y+h),theme.getAccentU32(0.85f),theme.cornerRadius);
    dl->AddText(ImVec2(pos.x+12,pos.y+8),*enabled?theme.getAccentU32():theme.getTextU32(),name);
    if(desc&&*desc)dl->AddText(ImVec2(pos.x+12,pos.y+26),theme.getTextSecondaryU32(),desc);
        const float tw=36.f,th=16.f,knobR=6.f;
    float tx=pos.x+w-tw-8,ty=pos.y+(h-th)*0.5f;
    float& tt=anim.toggleAnims[id];
    tt=tt+(*enabled?1.f:0.f-tt)*std::min(1.f,ImGui::GetIO().DeltaTime*14.f);
    tt=std::clamp(tt,0.f,1.f);
    ImVec4 tc=lerpColor(ImVec4(0.22f,0.22f,0.22f,1.f),withAlpha(theme.getAccent(),0.75f),tt);
    dl->AddRectFilled(ImVec2(tx,ty),ImVec2(tx+tw,ty+th),toU32(tc),th*0.5f);
    float capR=th*0.5f;
    float knobCX=tx+capR+(tw-2.f*capR)*tt;
    float knobCY=ty+capR;
    dl->AddCircleFilled(ImVec2(knobCX,knobCY),knobR,toU32(lerpColor(ImVec4(0.55f,0.55f,0.55f,1.f),ImVec4(1.f,1.f,1.f,1.f),tt)));
    if(clicked)*enabled=!*enabled;
    return *enabled;}

bool ModuleCardBegin(const char* name,const char* desc,bool* enabled,
    ThemeEngine& theme,AnimationState& anim,int* keybind){
    ModuleCard(name,desc,enabled,theme,anim,keybind);
    if(*enabled){ImGui::SetCursorPosX(ImGui::GetCursorPosX()+8);ImGui::Indent(8);}
    return *enabled;}
void ModuleCardEnd(){ImGui::Unindent(8);ImGui::Dummy(ImVec2(0,4));}

void StatusBadge(const char* text,ImVec4 color){
    ImVec2 ts=ImGui::CalcTextSize(text);
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float pad=6.f,h=ts.y+pad*2,w=ts.x+pad*2;
    ImDrawList* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos,ImVec2(pos.x+w,pos.y+h),toU32(withAlpha(color,0.18f)),999.f);
    dl->AddRect(pos,ImVec2(pos.x+w,pos.y+h),toU32(withAlpha(color,0.6f)),999.f,0,1.f);
    dl->AddText(ImVec2(pos.x+pad,pos.y+pad),toU32(color),text);
    ImGui::Dummy(ImVec2(w,h));}

bool PillButton(const char* label,bool active,float width,ThemeEngine& theme,AnimationState& anim){
    ImGuiID id=ImGui::GetID(label);
    float& t=anim.hoverAnims[id];
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float h=32.f;
    ImGui::InvisibleButton(label,ImVec2(width,h));
    bool clicked=ImGui::IsItemClicked();
    bool hovered=ImGui::IsItemHovered();
    t=smoothStep(t,hovered?1.f:0.f,12.f,ImGui::GetIO().DeltaTime);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec4 bg=active?withAlpha(theme.getAccent(),0.22f):lerpColor(withAlpha(theme.cardColor,0.5f),withAlpha(theme.getAccent(),0.1f),t);
    dl->AddRectFilled(pos,ImVec2(pos.x+width,pos.y+h),toU32(bg),h*0.5f);
    dl->AddRect(pos,ImVec2(pos.x+width,pos.y+h),theme.getAccentU32(active?0.8f:0.25f),h*0.5f,0,active?1.2f:0.5f);
    ImVec2 ts=ImGui::CalcTextSize(label);
    ImU32 tc=active?theme.getAccentU32():hovered?theme.getTextU32():theme.getTextSecondaryU32();
    dl->AddText(ImVec2(pos.x+(width-ts.x)*0.5f,pos.y+(h-ts.y)*0.5f),tc,label);
    return clicked;}

void KeybindButton(const char* label,int* keyCode,ThemeEngine& theme,AnimationState& anim){
    ImGui::Text("%s",label);
    ImGui::SameLine();
    char btnLabel[64];
    snprintf(btnLabel,sizeof(btnLabel),"%s##kb_%s",getKeyName(*keyCode).c_str(),label);
    bool isRebinding=(MenuInterface::get()->rebindTarget==keyCode);
    if(isRebinding){
        ImGui::PushStyleColor(ImGuiCol_Button,withAlpha(theme.getAccent(),0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,withAlpha(theme.getAccent(),0.4f));
        if(ImGui::Button("Press key...",ImVec2(110,0))){
            MenuInterface::get()->rebindTarget=nullptr;}
        ImGui::PopStyleColor(2);
    }else{
        if(StyledButton(btnLabel,ImVec2(110,0),theme,anim)){
            MenuInterface::get()->rebindTarget=keyCode;}}}
}

void MenuInterface::drawBackdrop(){
    if(anim.openProgress<=0.f)return;
    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin("##backdrop",nullptr,
        ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl=ImGui::GetWindowDrawList();
    float a=anim.easeOutCubic(anim.openProgress)*0.35f;
    dl->AddRectFilled(vp->Pos,ImVec2(vp->Pos.x+vp->Size.x,vp->Pos.y+vp->Size.y),IM_COL32(0,0,0,(int)(a*255)));
    if(ambientWavesEnabled)drawAmbientWaves(dl,vp->Pos,ImVec2(vp->Pos.x+vp->Size.x,vp->Pos.y+vp->Size.y));
    ImGui::End();}

void MenuInterface::drawAmbientWaves(ImDrawList* dl,ImVec2 mn,ImVec2 mx){
    ambientTime+=ImGui::GetIO().DeltaTime*0.3f;
    ImVec4 acc=theme.getAccent();
    float w=mx.x-mn.x,h=mx.y-mn.y;
    for(int i=0;i<3;i++){
        float phase=(float)i*2.094f;
        float amp=h*0.06f,freq=1.5f+i*0.5f;
        float baseY=mn.y+h*(0.3f+i*0.2f);
        const int segs=80;
        ImVec2 prev;
        for(int j=0;j<=segs;j++){
            float fx=(float)j/segs;
            float x=mn.x+fx*w;
            float y=baseY+amp*std::sin(fx*freq*3.14159f*2+ambientTime+phase);
            ImVec2 cur(x,y);
            if(j>0)dl->AddLine(prev,cur,IM_COL32((int)(acc.x*255),(int)(acc.y*255),(int)(acc.z*255),(int)(18.f*(1.f-i*0.25f)*anim.openProgress)),1.f);
            prev=cur;}}
}

void MenuInterface::drawTitleBar(){
    auto* engine=GucciEngine::get();
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 wp=ImGui::GetWindowPos(),ws=ImGui::GetWindowSize();
    float barH=52.f;
        ImVec4 barBg=withAlpha(theme.bgColor,0.3f);barBg.w=0.f;
    dl->AddRectFilled(wp,ImVec2(wp.x+ws.x,wp.y+barH),toU32(barBg),theme.cornerRadius,ImDrawFlags_RoundCornersTop);
        ImVec2 lc(wp.x+24,wp.y+barH*0.5f);
    float lr=10.f;
    ImVec4 acc=theme.getAccent();
    dl->AddQuad(ImVec2(lc.x,lc.y-lr),ImVec2(lc.x+lr,lc.y),ImVec2(lc.x,lc.y+lr),ImVec2(lc.x-lr,lc.y),theme.getAccentU32(0.9f),1.5f);
    dl->AddQuadFilled(ImVec2(lc.x,lc.y-lr),ImVec2(lc.x+lr,lc.y),ImVec2(lc.x,lc.y),ImVec2(lc.x-lr,lc.y),theme.getAccentU32(0.18f));
        const char* botName=
        (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?
            "ToosiiBot":
        (activeTheme==THEME_JA)?"JaBot":
        (activeTheme==THEME_GIDDEY)?"GiddeyBot":
        (activeTheme==THEME_BAM)?"BamBot":
        (activeTheme==THEME_SEXYY)?"SexyyBot":
        (activeTheme==THEME_JUICE)?"JuiceBot":
        (activeTheme==THEME_BUTLER)?"ButlerBot":"GucciBot";
    ImVec2 npos(wp.x+40,wp.y+10);
    if(fontHeading)ImGui::PushFont(fontHeading);
    dl->AddText(npos,theme.getAccentU32(),botName);
    if(fontHeading)ImGui::PopFont();
        const char* sub=
        (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?
            "v" MOD_VERSION "  -  Running routes. Dropping passes.":
        (activeTheme==THEME_JA)?
            "v" MOD_VERSION "  -  They can't stop me. I'm different.":
        (activeTheme==THEME_GIDDEY)?
            "v" MOD_VERSION "  -  G'day. I'm open, apparently.":
        (activeTheme==THEME_BAM)?
            "v" MOD_VERSION "  -  BITCH IM KOBEEE!!!":
        (activeTheme==THEME_SEXYY)?
            "v" MOD_VERSION "  -  Frame perfect. Goes stupid. Skee yee.":
        (activeTheme==THEME_JUICE)?
            "v" MOD_VERSION "  -  That's tuff. Brrr.":
        (activeTheme==THEME_BUTLER)?
            "v" MOD_VERSION "  -  Playoff Jimmy mode: always on.":
        "v" MOD_VERSION "  -  Frame perfect. GBR6. Brrr.";
    ImVec2 spos(wp.x+40,wp.y+30);
    if(fontSmall)ImGui::PushFont(fontSmall);
    dl->AddText(spos,theme.getTextSecondaryU32(),sub);
    if(fontSmall)ImGui::PopFont();
        dl->AddLine(ImVec2(wp.x,wp.y+barH),ImVec2(wp.x+ws.x,wp.y+barH),theme.getAccentU32(0.15f),1.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+barH+2);}

void MenuInterface::switchTab(int newTab){
    if(newTab==activeTab)return;
    previousTab=activeTab;activeTab=newTab;
    anim.tabTransition=0.f;anim.transitionFromTab=previousTab;}

void MenuInterface::drawTabBar(){
        ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float width=ImGui::GetContentRegionAvail().x;
        const char* names[]={"Macro","Render","Clicks","Autoclicker","Hacks","Indicators","JMF","Trainer","HUD","Settings","Credits"};
    const int N=11;
    float tabW=width/N,tabH=34.f;
    float dt=ImGui::GetIO().DeltaTime;
    if(tabIndicatorX<0)tabIndicatorX=pos.x+activeTab*tabW;
    tabIndicatorX=smoothStep(tabIndicatorX,pos.x+activeTab*tabW,14.f+anim.animSpeed*0.7f,dt);
    for(int i=0;i<N;i++){
        ImVec2 tMin(pos.x+i*tabW,pos.y),tMax(tMin.x+tabW,pos.y+tabH);
        char tid[32];snprintf(tid,sizeof(tid),"##tab%d",i);
        ImGui::SetCursorScreenPos(tMin);
        ImGui::InvisibleButton(tid,ImVec2(tabW,tabH));
        bool hov=ImGui::IsItemHovered();
        if(ImGui::IsItemClicked())switchTab(i);
        if(fontSmall)ImGui::PushFont(fontSmall);
        ImU32 tc=(activeTab==i)?theme.getAccentU32(0.98f)
            :(i==6)?IM_COL32(200,175,90,190) // Jupiter tab stays warm gold even when inactive
            :(hov?theme.getTextU32():theme.getTextSecondaryU32());
        if(i==6&&activeTab==6){
            // Full name while open, wrapped to fit the tab's own column --
            // greedy word-wrap so it adapts to whatever the tab width is.
            const char* full="Nigel's Jupiter My Favourite Trainer";
            std::vector<std::string> words; {
                std::string w; for(const char* p=full;;++p){
                    if(*p==' '||*p==0){if(!w.empty())words.push_back(w);w.clear();if(*p==0)break;}
                    else w.push_back(*p);
                }
            }
            std::vector<std::string> lines; std::string cur;
            for(auto& w:words){
                std::string trial=cur.empty()?w:(cur+" "+w);
                if(ImGui::CalcTextSize(trial.c_str()).x<=tabW-6.f||cur.empty())cur=trial;
                else{lines.push_back(cur);cur=w;}
            }
            if(!cur.empty())lines.push_back(cur);
            float lineH=ImGui::GetFontSize();
            float totalH=lineH*(float)lines.size();
            float ly=tMin.y+(tabH-totalH)*0.5f;
            for(auto& ln:lines){
                ImVec2 ts=ImGui::CalcTextSize(ln.c_str());
                dl->AddText(ImVec2(tMin.x+(tabW-ts.x)*0.5f,ly),tc,ln.c_str());
                ly+=lineH;
            }
        } else {
            ImVec2 ts=ImGui::CalcTextSize(names[i]);
            ImVec2 tp(tMin.x+(tabW-ts.x)*0.5f,tMin.y+(tabH-ts.y)*0.5f);
            dl->AddText(tp,tc,names[i]);
        }
        if(fontSmall)ImGui::PopFont();}
        float indW=tabW*0.5f,indX=tabIndicatorX+(tabW-indW)*0.5f;
    dl->AddRectFilled(ImVec2(indX,pos.y+tabH-2),ImVec2(indX+indW,pos.y+tabH),theme.getAccentU32(0.92f),2.f);
    dl->AddLine(ImVec2(pos.x,pos.y+tabH),ImVec2(pos.x+width,pos.y+tabH),theme.getAccentU32(0.15f),1.f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x,pos.y+tabH+6));}

void MenuInterface::drawMainSubTabBar(){
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 pos=ImGui::GetCursorScreenPos();
    float width=ImGui::GetContentRegionAvail().x;
    const char* sub[]={"Replay","Tools","Hacks"};
    const int SN=3;
    float subW=width/SN,subH=30.f;
    float dt=ImGui::GetIO().DeltaTime;
    static float subIndX=-1.f;
    if(subIndX<0)subIndX=pos.x+mainSubTab*subW;
    subIndX=smoothStep(subIndX,pos.x+mainSubTab*subW,14.f+anim.animSpeed*0.7f,dt);
    for(int i=0;i<SN;i++){
        ImVec2 tMin(pos.x+i*subW,pos.y),tMax(tMin.x+subW,pos.y+subH);
        char tid[32];snprintf(tid,sizeof(tid),"##stab%d",i);
        ImGui::SetCursorScreenPos(tMin);
        ImGui::InvisibleButton(tid,ImVec2(subW,subH));
        bool hov=ImGui::IsItemHovered();
        if(ImGui::IsItemClicked())mainSubTab=i;
        if(fontSmall)ImGui::PushFont(fontSmall);
        ImVec2 ts=ImGui::CalcTextSize(sub[i]);
        ImVec2 tp(tMin.x+(subW-ts.x)*0.5f,tMin.y+(subH-ts.y)*0.5f);
        ImU32 tc=(mainSubTab==i)?theme.getAccentU32(0.98f):(hov?theme.getTextU32():theme.getTextSecondaryU32());
        dl->AddText(tp,tc,sub[i]);
        if(fontSmall)ImGui::PopFont();}
    float indW=subW*0.5f,indX=subIndX+(subW-indW)*0.5f;
    dl->AddRectFilled(ImVec2(indX,pos.y+subH-2),ImVec2(indX+indW,pos.y+subH),theme.getAccentU32(0.92f),2.f);
    dl->AddLine(ImVec2(pos.x,pos.y+subH),ImVec2(pos.x+width,pos.y+subH),theme.getAccentU32(0.15f),1.f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x,pos.y+subH+6));}

void MenuInterface::drawStatusBar(){
    auto* engine=GucciEngine::get();
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 wp=ImGui::GetWindowPos(),ws=ImGui::GetWindowSize();
    float padX=ImGui::GetStyle().WindowPadding.x,barH=30.f;
    float barY=wp.y+ws.y-barH-10.f;
    ImVec2 bMin(wp.x+padX,barY),bMax(wp.x+ws.x-padX,barY+barH);
    drawSolidRect(dl,bMin,bMax,theme.cornerRadius,theme,1.f);
    if(fontSmall)ImGui::PushFont(fontSmall);
    char buf[256];
    int tick=PlayLayer::get()?0:0;
    snprintf(buf,sizeof(buf),"TPS: %.0f    Speed: %.2fx    Tick: %d",
        engine->updater.m_tps,engine->updater.m_speedhack,tick);
    ImVec2 ts=ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(wp.x+padX+12,barY+(barH-ts.y)*0.5f),theme.getTextSecondaryU32(),buf);
        const char* brand=
        (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?
            "Open!":
        (activeTheme==THEME_JA)?"IYKYK!":
        (activeTheme==THEME_GIDDEY)?"Crikey!":
        (activeTheme==THEME_BAM)?"83 pts.":
        (activeTheme==THEME_SEXYY)?"Skee yee.":
        (activeTheme==THEME_JUICE)?"Tuff.":
        (activeTheme==THEME_BUTLER)?"Playoff Jimmy.":"Brrr.";
    ImVec2 bts=ImGui::CalcTextSize(brand);
    dl->AddText(ImVec2(wp.x+ws.x-padX-bts.x-12,barY+(barH-bts.y)*0.5f),theme.getAccentU32(0.6f),brand);
    if(fontSmall)ImGui::PopFont();}

void MenuInterface::drawTabContent(){
    if(frameEditor.isActive()){
        if(fontBody)ImGui::PushFont(fontBody);
        frameEditor.draw(*this);
        if(fontBody)ImGui::PopFont();
        return;}
    float t=anim.easeOutCubic(anim.tabTransition);
    float offsetY=(1.f-t)*14.f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+offsetY);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,t);
    if(fontBody)ImGui::PushFont(fontBody);
    switch(activeTab){
        case 0:
            drawMainSubTabBar();
            switch(mainSubTab){
                case 0:drawReplayTab();break;
                case 1:drawToolsTab();break;
                case 2:drawHacksTab();break;}
            break;
        case 1:drawRenderTab();break;
        case 2:drawClicksTab();break;
        case 3:drawAutoclickerTab();break;
        case 4:drawMoreHacksTab();break;
        case 5:drawIndicatorsTab();break;
        case 6:drawJupiterTab();break;
        case 7:drawTrainerTab();break;
        case 8:drawHudTab();break;
        case 9:drawSettingsTab();break;
        case 10:drawCreditsTab();break;}
    if(fontBody)ImGui::PopFont();
    ImGui::PopStyleVar();}

void MenuInterface::drawMainWindow(){
    auto* engine=GucciEngine::get();
    float t=anim.easeOutCubic(anim.openProgress);
    if(t<=0.f)return;

        // Jupiter tab: reskin the WHOLE window's theme (title bar, tab bar, status
    // bar, every widget) for as long as this tab is active, not just its own
    // content -- restored at the end of this function either way.
    bool jupiterActive=(activeTab==6);
    ThemeEngine savedTheme=theme;
    if(jupiterActive){
        // Nigel's own two colors from his mockup: #100680 navy, #FCF550 gold.
        theme.accentColor   = ImVec4(0.988f,0.961f,0.314f,1.f);
        // Full takeover, not a translucent menu -- ignore the user's general
        // bg-opacity slider entirely rather than inherit it (that's what was
        // still reading as ~80%: SetNextWindowBgAlpha uses theme.bgOpacity,
        // which defaults to 0.96 and can be set as low as 0.5).
        theme.bgColor       = ImVec4(0.063f,0.024f,0.502f,1.f);
        theme.cardColor     = ImVec4(0.09f,0.05f,0.58f,1.f);
        theme.textPrimary   = ImVec4(0.988f,0.961f,0.314f,1.f);
        theme.textSecondary = ImVec4(0.70f,0.66f,0.85f,1.f);
    }
    theme.applyToImGuiStyle();
    if(jupiterActive){
        // Not a themed box on the screen -- the screen. Full viewport takeover
        // for as long as this tab is open; snaps back to the normal centered
        // window the instant you switch away.
        auto* vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos,ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->Size,ImGuiCond_Always);
    } else {
        ImVec2 center=ImGui::GetMainViewport()->GetCenter();
        if(!windowPosInitialized){
            ImGui::SetNextWindowPos(ImVec2(center.x-windowSize.x*0.5f,center.y-windowSize.y*0.5f),ImGuiCond_Always);
            windowPosInitialized=true;
        }
        ImGui::SetNextWindowSize(windowSize,ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha((jupiterActive?1.f:theme.bgOpacity)*t);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,t);
        ImGui::Begin("##GucciBot",nullptr,
        ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoTitleBar);
        applyBigBrrrBounce(jupiterActive);
        if(!jupiterActive){
        // Drag handle -- meaningless once the window IS the viewport, so skipped
        // entirely in Jupiter's full-screen takeover.
        ImVec2 wp=ImGui::GetWindowPos();
        ImVec2 ws=ImGui::GetWindowSize();
        ImDrawList* fdl=ImGui::GetForegroundDrawList();
        ImVec2 dragMin(wp.x+ws.x*0.35f,wp.y+3);
        ImVec2 dragMax(wp.x+ws.x*0.65f,wp.y+6);
                float shimT=(float)ImGui::GetTime()*1.2f;
        for(int i=0;i<3;i++){
            float phase=(float)i*0.4f;
            float alpha=0.18f+0.12f*std::sin(shimT+phase);
            float x1=dragMin.x+(dragMax.x-dragMin.x)*((float)i/3.f);
            float x2=dragMin.x+(dragMax.x-dragMin.x)*((float)(i+1)/3.f);
            fdl->AddRectFilled(ImVec2(x1,dragMin.y),ImVec2(x2,dragMax.y),
                theme.getAccentU32(alpha),2.f);}
        fdl->AddRectFilled(dragMin,dragMax,theme.getAccentU32(0.35f),2.f);
    }
    windowPos=ImGui::GetWindowPos();
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 wp=windowPos,ws=ImGui::GetWindowSize();
    if(jupiterActive&&!jupiterClickBarPageOpen){
        // Full-screen now, so there's no "past the edge" to bleed onto -- that
        // budget goes into a denser backdrop instead (see drawJupiterBackdrop).
        // Suppressed entirely on the Click Trainer page -- per Nigel, that page
        // should read as a clean functional tool, not compete with the artwork.
        drawJupiterBackdrop(dl,wp,ws,(float)ImGui::GetTime());
    } else if(!jupiterActive){
        dl->AddRect(wp,ImVec2(wp.x+ws.x,wp.y+ws.y),theme.getAccentU32(0.35f),theme.cornerRadius,0,1.5f);
    }
    if(!jupiterActive)drawTitleBar(); // GucciBot branding suppressed entirely on Jupiter
    ImGui::SetNextWindowContentSize(ImVec2(0,0));
    float contentH=ws.y-(jupiterActive?14.f:52.f)-40-14;
    if(jupiterActive)ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(0,0,0,0));
    ImGui::BeginChild("##content",ImVec2(-1,contentH),false,ImGuiWindowFlags_NoScrollbar);
    drawTabBar();
    ImGui::BeginChild("##tabcontent",ImVec2(-1,-1),false);
    drawTabContent();
    ImGui::EndChild();
    ImGui::EndChild();
    if(jupiterActive)ImGui::PopStyleColor();
    if(!jupiterActive)drawStatusBar(); // TPS/tick readout + brand text also suppressed
    ImGui::End();
    ImGui::PopStyleVar();
    if(jupiterActive)theme=savedTheme;}

void MenuInterface::drawMegaHackWindow(){
    float t=anim.easeOutCubic(anim.openProgress);
    if(t<=0.f)return;

    bool jupiterActive=(activeTab==6);
    ThemeEngine savedTheme=theme;
    if(jupiterActive){
        // Nigel's own two colors from his mockup: #100680 navy, #FCF550 gold.
        theme.accentColor   = ImVec4(0.988f,0.961f,0.314f,1.f);
        // Full takeover, not a translucent menu -- ignore the user's general
        // bg-opacity slider entirely rather than inherit it (that's what was
        // still reading as ~80%: SetNextWindowBgAlpha uses theme.bgOpacity,
        // which defaults to 0.96 and can be set as low as 0.5).
        theme.bgColor       = ImVec4(0.063f,0.024f,0.502f,1.f);
        theme.cardColor     = ImVec4(0.09f,0.05f,0.58f,1.f);
        theme.textPrimary   = ImVec4(0.988f,0.961f,0.314f,1.f);
        theme.textSecondary = ImVec4(0.70f,0.66f,0.85f,1.f);
    }
    theme.applyToImGuiStyle();
    if(jupiterActive){
        auto* vp=ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos,ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->Size,ImGuiCond_Always);
    } else {
        ImVec2 center=ImGui::GetMainViewport()->GetCenter();
        ImVec2 mhSize(620.f,400.f);
        ImGui::SetNextWindowPos(ImVec2(center.x-mhSize.x*0.5f,center.y-mhSize.y*0.5f),ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(mhSize,ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,t);
    ImGui::Begin("##GucciBotMH",nullptr,
        ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoTitleBar);
    applyBigBrrrBounce(jupiterActive);
    windowPos=ImGui::GetWindowPos();
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 wp=windowPos,ws=ImGui::GetWindowSize();
    const float railW=150.f,headH=44.f,footH=30.f,rnd=6.f;
    // MegaHack skin's own fixed dark palette, unrelated to theme.bgColor --
    // for the JMF full takeover, force Nigel's navy at full opacity here too
    // instead of leaving the old near-black show through underneath it.
    ImU32 bgMain=jupiterActive?IM_COL32(16,6,128,255):IM_COL32(18,19,26,(int)(243*t));
    ImU32 bgRail=jupiterActive?IM_COL32(16,6,128,255):IM_COL32(13,14,19,(int)(248*t));
    ImU32 bgHead=jupiterActive?IM_COL32(16,6,128,255):IM_COL32(22,24,32,(int)(248*t));
    dl->AddRectFilled(wp,ImVec2(wp.x+ws.x,wp.y+ws.y),bgMain,rnd);
    dl->AddRectFilled(wp,ImVec2(wp.x+railW,wp.y+ws.y),bgRail,rnd,ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(wp.x+railW,wp.y),ImVec2(wp.x+ws.x,wp.y+headH),bgHead,rnd,ImDrawFlags_RoundCornersTopRight);
    if(jupiterActive&&!jupiterClickBarPageOpen){
        drawJupiterBackdrop(dl,wp,ws,(float)ImGui::GetTime());
    } else if(!jupiterActive){
        dl->AddRect(wp,ImVec2(wp.x+ws.x,wp.y+ws.y),theme.getAccentU32(0.45f),rnd,0,1.f);
    }
    dl->AddLine(ImVec2(wp.x+railW,wp.y),ImVec2(wp.x+railW,wp.y+ws.y),theme.getAccentU32(0.12f),1.f);
    dl->AddLine(ImVec2(wp.x+railW,wp.y+headH),ImVec2(wp.x+ws.x,wp.y+headH),theme.getAccentU32(0.10f),1.f);
    if(!jupiterActive){
        const char* title=(activeTheme==THEME_TOOSII)?"TOOSIIBOT":"GUCCIBOT";
        float titleW=0.f;
        if(fontHeading)ImGui::PushFont(fontHeading);
        titleW=ImGui::CalcTextSize(title).x;
        dl->AddText(ImVec2(wp.x+railW+14,wp.y+(headH-ImGui::GetFontSize())*0.5f),
            theme.getAccentU32(0.96f),title);
        if(fontHeading)ImGui::PopFont();
        if(fontSmall)ImGui::PushFont(fontSmall);
        dl->AddText(ImVec2(wp.x+railW+14+titleW+10,wp.y+headH*0.5f-5),
            theme.getTextSecondaryU32(),"v" MOD_VERSION "  mega edition. brrr.");
        if(fontSmall)ImGui::PopFont();
        if(fontHeading)ImGui::PushFont(fontHeading);
        dl->AddText(ImVec2(wp.x+16,wp.y+12),theme.getAccentU32(0.92f),"GB");
        if(fontHeading)ImGui::PopFont();
    }
        const char* names[]={"Macro","Render","Clicks","Autoclicker","Hacks","Indicators","JMF","Trainer","HUD","Settings","Credits"};
    float rowH=34.f,railTop=headH+10.f;
    for(int i=0;i<11;i++){
        ImVec2 rMin(wp.x,wp.y+railTop+i*rowH),rMax(wp.x+railW,rMin.y+rowH);
        char rid[24];snprintf(rid,sizeof(rid),"##mhTab%d",i);
        ImGui::SetCursorScreenPos(rMin);
        ImGui::InvisibleButton(rid,ImVec2(railW,rowH));
        bool hov=ImGui::IsItemHovered();
        if(ImGui::IsItemClicked())switchTab(i);
        bool act=(activeTab==i);
        if(act)dl->AddRectFilled(rMin,rMax,theme.getAccentU32(0.10f));
        else if(hov)dl->AddRectFilled(rMin,rMax,IM_COL32(255,255,255,10));
        if(act)dl->AddRectFilled(rMin,ImVec2(rMin.x+3,rMax.y),theme.getAccentU32(0.95f));
        if(fontBody)ImGui::PushFont(fontBody);
        ImU32 tc=act?theme.getAccentU32(0.98f)
            :(i==6)?IM_COL32(200,175,90,190)
            :(hov?theme.getTextU32():theme.getTextSecondaryU32());
        if(i==6&&act){
            const char* full="Nigel's Jupiter My Favourite Trainer";
            std::vector<std::string> words; {
                std::string w; for(const char* p=full;;++p){
                    if(*p==' '||*p==0){if(!w.empty())words.push_back(w);w.clear();if(*p==0)break;}
                    else w.push_back(*p);
                }
            }
            std::vector<std::string> lines; std::string cur;
            float maxW=railW-22.f;
            for(auto& w:words){
                std::string trial=cur.empty()?w:(cur+" "+w);
                if(ImGui::CalcTextSize(trial.c_str()).x<=maxW||cur.empty())cur=trial;
                else{lines.push_back(cur);cur=w;}
            }
            if(!cur.empty())lines.push_back(cur);
            float lineH=ImGui::GetFontSize();
            float ly=rMin.y+(rowH-lineH*(float)lines.size())*0.5f;
            for(auto& ln:lines){dl->AddText(ImVec2(rMin.x+16,ly),tc,ln.c_str());ly+=lineH;}
        } else {
            dl->AddText(ImVec2(rMin.x+16,rMin.y+(rowH-ImGui::GetFontSize())*0.5f),tc,names[i]);
        }
        if(fontBody)ImGui::PopFont();}
        ImGui::SetCursorScreenPos(ImVec2(wp.x+railW+12,wp.y+headH+8));
    if(jupiterActive)ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(0,0,0,0));
    ImGui::BeginChild("##mhContent",ImVec2(ws.x-railW-24,ws.y-headH-footH-16),false,ImGuiWindowFlags_NoScrollbar);
    drawTabContent();
    ImGui::EndChild();
    if(jupiterActive)ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(wp.x+railW+12,wp.y+ws.y-footH+2));
    if(!jupiterActive)drawStatusBar();
    ImGui::End();
    ImGui::PopStyleVar();
    if(jupiterActive)theme=savedTheme;}

// Small always-visible corner panel instead of the full tabbed window --
// same idea as yBot's compact bot window: record/play, TPS/speed, frame
// step, and the handful of toggles you'd actually want mid-attempt, small
// enough to leave open while actually playing without blocking the level.
// Positioned like the existing HUD overlays (displayGameplayHUD etc. --
// SetNextWindowPos with ImGuiCond_Always, corner-anchored, recomputed every
// frame), but unlike those this one takes real input, so it can't use
// ImGuiWindowFlags_NoInputs/NoNav.
void MenuInterface::drawCompactWindow(){
    auto* engine=GucciEngine::get();
    auto* upd=&engine->updater;
    auto* mod=Mod::get();

    float t=anim.easeOutCubic(anim.openProgress);
    if(t<=0.f)return;

    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+10,vp->Pos.y+10),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240,0),ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,t);
    ImGui::Begin("##gbCompact",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);

    if(fontHeading)ImGui::PushFont(fontHeading);
    ImGui::TextColored(theme.getAccent(),"GucciBot");
    if(fontHeading)ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowWidth()-58);
    if(Widgets::StyledButton("Full",ImVec2(48,22),theme,anim,4.f))compactMode=false;
    ImGui::Separator();

    const char* modeStr=engine->isRecording()?"RECORDING":engine->isPlaying()?"PLAYING":"IDLE";
    ImVec4 modeCol=engine->isRecording()?ImVec4(1.f,0.3f,0.3f,1.f):engine->isPlaying()?ImVec4(0.3f,1.f,0.3f,1.f):theme.textSecondary;
    ImGui::TextColored(modeCol,"%s",modeStr);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text(" f=%u  %.0f TPS  %.2fx",upd->getFrame(),upd->m_tps,upd->m_speedhack);
    ImGui::PopStyleColor();
    if(!engine->replayName.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("%s",engine->replayName.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,4));

    float bw=(ImGui::GetContentRegionAvail().x-6)/2.f;
    if(engine->isRecording()){
        if(Widgets::StyledButton("Stop Recording",ImVec2(-1,26),theme,anim))engine->setMode(GucciEngine::Mode::Idle);
    } else if(engine->isPlaying()){
        if(Widgets::StyledButton("Stop Playback",ImVec2(-1,26),theme,anim))engine->setMode(GucciEngine::Mode::Idle);
    } else {
        if(Widgets::StyledButton("Record",ImVec2(bw,26),theme,anim))engine->setMode(GucciEngine::Mode::Recording);
        ImGui::SameLine(0,6);
        bool canPlay=!engine->replay.m_actionAtom.m_actions.empty();
        if(!canPlay)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
        bool playClicked=Widgets::StyledButton("Play",ImVec2(bw,26),theme,anim);
        if(!canPlay)ImGui::PopStyleVar();
        if(playClicked&&canPlay)engine->setMode(GucciEngine::Mode::Playing);
    }
    ImGui::Dummy(ImVec2(0,6));
    ImGui::Separator();

    ImGui::SetNextItemWidth(bw);
    ImGui::InputFloat("##ctps",&compactTempTickRate,0,0,"%.0f TPS");
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Apply##ctps",ImVec2(bw,24),theme,anim)){
        if(!PlayLayer::get()||!engine->isPlaying()){
            upd->m_tps=compactTempTickRate;
            mod->setSavedValue("eng_tick_rate",(float)upd->m_tps);
        }
    }
    ImGui::SetNextItemWidth(bw);
    ImGui::InputFloat("##cspd",&compactTempGameSpeed,0,0,"%.2fx");
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Apply##cspd",ImVec2(bw,24),theme,anim))upd->m_speedhack=compactTempGameSpeed;
    ImGui::Dummy(ImVec2(0,6));

    if(Widgets::ToggleSwitch("Frame Advance",&upd->m_paused,theme,anim)){}
    if(upd->m_paused){
        if(Widgets::StyledButton("<< Back",ImVec2(bw,24),theme,anim,4.f)){
            if(upd->m_backwardsStepping)upd->backwardsStep(1);
        }
        ImGui::SameLine(0,6);
        if(Widgets::StyledButton("Step >>",ImVec2(bw,24),theme,anim,4.f))upd->m_stepOnce_=true;
    }
    ImGui::Dummy(ImVec2(0,4));
    ImGui::Separator();

    if(Widgets::ToggleSwitch("Noclip",&engine->noclipEnabled,theme,anim))
        mod->setSavedValue("hack_noclip",engine->noclipEnabled);
    if(Widgets::ToggleSwitch("Layout Mode",&engine->layoutMode,theme,anim))
        mod->setSavedValue("hack_layout_mode",engine->layoutMode);
    if(Widgets::ToggleSwitch("Show Hitboxes",&engine->showHitboxes,theme,anim))
        mod->setSavedValue("hack_hitboxes",engine->showHitboxes);
    if(Widgets::ToggleSwitch("No Mirror",&engine->noMirrorEffect,theme,anim))
        mod->setSavedValue("hack_no_mirror",engine->noMirrorEffect);
    if(Widgets::ToggleSwitch("Swap Player Inputs",&engine->replay.m_mirrorInputs,theme,anim)){}

    ImGui::End();
    ImGui::PopStyleVar();
}

void MenuInterface::drawReplayTab(){
    auto* engine=GucciEngine::get();
        if((activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE))
        Widgets::GucciQuote("\"What's cover 1?\"","-- Toosii, asking the cornerback",theme);
    else if(activeTheme==THEME_JA)
        Widgets::GucciQuote("\"Nobody can replay what I just did. Nobody.\""," -- Ja Morant",theme);
    else if(activeTheme==THEME_GIDDEY)
        Widgets::GucciQuote("\"6 was a little high; I was expecting to be in the 7-13 range.\"","-- Josh Giddey, on being the 6th pick",theme);
    else if(activeTheme==THEME_BAM)
        Widgets::GucciQuote("\"I don't record inputs. I record history. 83 points of it.\"","-- Bam, in the zone",theme);
    else if(activeTheme==THEME_SEXYY)
        Widgets::GucciQuote("\"I don't miss. Not a single frame. Skee yee.\"","-- Sexyy Red, probably",theme);
    else if(activeTheme==THEME_JUICE)
        Widgets::GucciQuote("\"I tested every frame. Every single one.\"","-- Juice, probably",theme);
    else if(activeTheme==THEME_BUTLER)
        Widgets::GucciQuote("\"Regular season replays don't count. I lock in for the playoffs.\"","-- Jimmy Butler, probably",theme);
    else
        Widgets::GucciQuote("\"I got so many replays I got files in my files.\"","-- Gucci Mane, probably",theme);
    Widgets::SectionHeader("Mode",theme);
    float pillW=(ImGui::GetContentRegionAvail().x-20)/3.f;
    static bool showFormatPopup=false;

        if(Widgets::PillButton("Disable",engine->isIdle(),pillW,theme,anim)){
        if(engine->isRecording()){
                    }
        engine->setMode(GucciEngine::Mode::Idle);
        engine->startPosWarning.clear();
    }
    ImGui::SameLine(0,10);

        if(Widgets::PillButton("Record",engine->isRecording(),pillW,theme,anim)){
        if(engine->isRecording()){
            engine->setMode(GucciEngine::Mode::Idle);
        } else if(engine->isPlaying()&&!engine->replay.m_actionAtom.empty()){
                                    if(engine->beginResumeRecording()){anim.closing=true;anim.opening=false;}
        } else {
            engine->replay.m_actionAtom.clear();
            engine->replay.m_pathSamples.clear();
            engine->replay.m_inputIndex = 0;
            engine->updater.resetFrame();
            engine->updater.m_frameOnLastAttempt = 0;
            engine->setMode(GucciEngine::Mode::Recording);
            anim.closing=true; anim.opening=false;
        }
    }
    ImGui::SameLine(0,10);

        bool playbackActive = engine->isPlaying();
    if(Widgets::PillButton("Playback",playbackActive,pillW,theme,anim)){
        if(playbackActive){
            engine->setMode(GucciEngine::Mode::Idle);
        } else if(!engine->replay.m_actionAtom.m_actions.empty()){
            engine->updater.resetFrame();
            engine->updater.m_frameOnLastAttempt = 0;
            engine->replay.m_inputIndex = 0;
            engine->setMode(GucciEngine::Mode::Playing);
            anim.closing=true; anim.opening=false;
        }
    }
            if(ImGui::BeginPopup("##FmtSel",ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        drawPopupChrome(*this,"Select Format");
        float bw=110.f;
                const char* nativeLabel=
            (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?".toosii":
            (activeTheme==THEME_JA)?".ja":
            (activeTheme==THEME_GIDDEY)?".giddey":(activeTheme==THEME_BAM)?".bam":(activeTheme==THEME_SEXYY)?".sexyy":(activeTheme==THEME_JUICE)?".juice":(activeTheme==THEME_BUTLER)?".butler":".brrr";
        if(Widgets::StyledButton(nativeLabel,ImVec2(bw,30),theme,anim,6.f)){
            if(PlayLayer::get())engine->setMode(GucciEngine::Mode::Recording);
            else engine->setMode(GucciEngine::Mode::Recording);
            ImGui::CloseCurrentPopup();}
                ImGui::EndPopup();}
    ImGui::Dummy(ImVec2(0,8));

        if(engine->isRecording()){
        size_t cnt = engine->replay.m_actionAtom.m_actions.size();
        const char* extLabel =
            (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?".toosii":
            (activeTheme==THEME_JA)?".ja":
            (activeTheme==THEME_GIDDEY)?".giddey":(activeTheme==THEME_BAM)?".bam":(activeTheme==THEME_SEXYY)?".sexyy":(activeTheme==THEME_JUICE)?".juice":(activeTheme==THEME_BUTLER)?".butler":".brrr";
        Widgets::StatusBadge("RECORDING",ImVec4(1.f,0.3f,0.3f,1.f));
        ImGui::SameLine();
        Widgets::StatusBadge(extLabel,getBRRTagColor());
        ImGui::SameLine();ImGui::Text("Actions: %zu",cnt);
        ImGui::Dummy(ImVec2(0,4));

                bool pending = engine->updater.m_canDie;
        if(pending){
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.8f,0.15f,0.15f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(1.f,0.25f,0.25f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.6f,0.1f,0.1f,1.f));
        }
        if(Widgets::StyledButton(
            pending?"Cancel Intentional Death":"Mark Next Death as Intentional",
            ImVec2(-1,30),theme,anim))
            engine->updater.m_canDie = !engine->updater.m_canDie;
        if(pending){
            ImGui::PopStyleColor(3);
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,0.7f,0.1f,1.f));
            ImGui::TextWrapped("Next player death will be recorded as intentional. Playback will continue to the next attempt.");
            ImGui::PopStyleColor();
        }

                ImGui::Dummy(ImVec2(0,4));
        if(Widgets::StyledButton("Record TPS Change",ImVec2(-1,28),theme,anim)){
            engine->recordTpsChange(engine->updater.m_tps);
        }
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Inserts a TPS change action at the current frame. Change TPS in the Tools tab first.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0,4));
        if(!macroNameReady){
            strncpy(macroNameBuffer,engine->replayName.c_str(),sizeof(macroNameBuffer)-1);
            macroNameBuffer[sizeof(macroNameBuffer)-1]=0;macroNameReady=true;}
        ImGui::Text("Macro Name:");ImGui::SetNextItemWidth(-1);
        if(ImGui::InputText("##recName",macroNameBuffer,sizeof(macroNameBuffer)))
            engine->replayName=macroNameBuffer;
        ImGui::Dummy(ImVec2(0,4));
        float bw=(ImGui::GetContentRegionAvail().x-10)/2.f;
        if(Widgets::StyledButton("Save Macro",ImVec2(bw,30),theme,anim)){
            if(!engine->replay.m_actionAtom.m_actions.empty()){
                auto savePath = Mod::get()->getSaveDir()/"replays"/(engine->replayName+extLabel);
                if(engine->replayBackupsEnabled) engine->replay.backupExisting(savePath);
                engine->replay.save(savePath);
                markReplayListDirty();refreshReplayListIfNeeded(true);
                ImGui::OpenPopup("SaveFrameWindows");
            }
        }
                if(ImGui::BeginPopupModal("SaveFrameWindows",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)){
            if(fontHeading)ImGui::PushFont(fontHeading);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
            ImGui::TextUnformatted("Macro Saved");
            ImGui::PopStyleColor();
            if(fontHeading)ImGui::PopFont();
            ImGui::Dummy(ImVec2(0,6));
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                                                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
            ImGui::TextUnformatted("Calculate frame windows for this macro? This replays the macro and simulates each click against the real engine to measure how tight it is. You must be in the level. May take a moment for long macros.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0,6));
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,0.8f,0.2f,1.f));
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
            ImGui::TextUnformatted("Known limitation: the replay itself is accurate now (2026-08-13, ground-truth-forced), but each click's tested timing shifts still fall back to real simulation past that click -- so windows near tricky slope sections may still read off.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0,10));
            float pbw=(ImGui::GetContentRegionAvail().x-8)/2.f;
            if(Widgets::StyledButton("Calculate",ImVec2(pbw,30),theme,anim,6.f)){
                engine->analyzeFrameWindows();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0,8);
            if(Widgets::StyledButton("Skip",ImVec2(pbw,30),theme,anim,6.f))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine(0,10);
        if(Widgets::StyledButton("Stop",ImVec2(bw,30),theme,anim)){
            engine->setMode(GucciEngine::Mode::Idle);macroNameReady=false;}
        ImGui::Dummy(ImVec2(0,4));
    } else macroNameReady=false;

        if(engine->isPlaying()&&!engine->replay.m_actionAtom.m_actions.empty()){
        size_t cnt=engine->replay.m_actionAtom.m_actions.size();
        std::string nm=engine->replayName;
        const char* extLabel2 =
            (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?".toosii":
            (activeTheme==THEME_JA)?".ja":
            (activeTheme==THEME_GIDDEY)?".giddey":(activeTheme==THEME_BAM)?".bam":(activeTheme==THEME_SEXYY)?".sexyy":(activeTheme==THEME_JUICE)?".juice":(activeTheme==THEME_BUTLER)?".butler":".brrr";
        Widgets::StatusBadge("PLAYING",ImVec4(0.3f,1.f,0.3f,1.f));
        ImGui::SameLine();
        Widgets::StatusBadge(extLabel2,getBRRTagColor());
        ImGui::SameLine();ImGui::Text("%s | Actions: %zu",nm.c_str(),cnt);
        ImGui::Dummy(ImVec2(0,4));
        if(Widgets::StyledButton("Stop Playback",ImVec2(-1,30),theme,anim))engine->setMode(GucciEngine::Mode::Idle);
        ImGui::Dummy(ImVec2(0,4));
        // Save + Calculate used to only be reachable while actively
        // recording -- loading an existing macro to re-run Calculate on it
        // (or just re-save it after e.g. the Frame Editor) had no path at
        // all. Same underlying calls the recording panel's Save/Calculate
        // popup uses, just exposed here too.
        float pbw2=(ImGui::GetContentRegionAvail().x-8)/2.f;
        if(Widgets::StyledButton("Save",ImVec2(pbw2,28),theme,anim)){
            auto savePath = Mod::get()->getSaveDir()/"replays"/(engine->replayName+extLabel2);
            if(engine->replayBackupsEnabled) engine->replay.backupExisting(savePath);
            engine->replay.save(savePath);
            markReplayListDirty();refreshReplayListIfNeeded(true);
            Notification::create("Macro saved",NotificationIcon::Success)->show();
        }
        ImGui::SameLine(0,8);
        bool canCalc=PlayLayer::get()!=nullptr;
        if(!canCalc)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
        bool calcClicked=Widgets::StyledButton("Calculate",ImVec2(pbw2,28),theme,anim);
        if(!canCalc)ImGui::PopStyleVar();
        if(calcClicked&&canCalc)engine->analyzeFrameWindows();
        if(!canCalc){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Enter the level to Calculate.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0,4));}
    if(!engine->startPosWarning.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,0.8f,0.2f,1.f));
        ImGui::TextWrapped("%s",engine->startPosWarning.c_str());
        ImGui::PopStyleColor();ImGui::Dummy(ImVec2(0,4));}

    Widgets::SectionHeader("Saved Replays",theme);
        static char macroFilter[64]="";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##macroSearch","Search macros...",macroFilter,sizeof(macroFilter));
    auto matchesFilter=[&](const std::string& nm)->bool{
        if(macroFilter[0]==0)return true;
        std::string a=nm,b=macroFilter;
        std::transform(a.begin(),a.end(),a.begin(),::tolower);
        std::transform(b.begin(),b.end(),b.begin(),::tolower);
        return a.find(b)!=std::string::npos;
    };
    refreshReplayListIfNeeded(false);
    float listPadY=8.f,listPadX=10.f;
    float listH=std::max(80.f,std::min(200.f,(float)engine->storedMacros.size()*28.f+listPadY*2));
    ImVec2 listPos=ImGui::GetCursorScreenPos();
    float listW=ImGui::GetContentRegionAvail().x;
    drawSolidRect(ImGui::GetWindowDrawList(),listPos,ImVec2(listPos.x+listW,listPos.y+listH),theme.cornerRadius,theme,0.55f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,0.f);
    ImGui::BeginChild("##MacroList",ImVec2(-1,listH),false);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+listPadX);
    ImGui::Dummy(ImVec2(0,listPadY));
    if(engine->storedMacros.empty()){
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+listPadX);
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,0.4f));
        const char* emptyMsg=(activeTheme==THEME_TOOSII)?
            "No saved replays -- get to work":"No saved replays -- get to work";
        ImGui::Text("%s",emptyMsg);
        ImGui::PopStyleColor();}
    auto macroCopy=engine->storedMacros;
    for(const auto& mn:macroCopy){
        if(!matchesFilter(mn))continue;
        bool isSel=(!engine->replay.m_actionAtom.m_actions.empty()&&!engine->isRecording()&&engine->replayName==mn);
        bool isIncompat=engine->incompatibleMacros.count(mn)>0;

        ImGui::PushID(mn.c_str());
        const float xBtnW=20.f,dotsBtnW=22.f,btnGap=3.f;
        float rowH=ImGui::GetTextLineHeight()+8.f,fullW=ImGui::GetContentRegionAvail().x;
        float rightReserved=xBtnW+dotsBtnW+btnGap+listPadX;
        std::string rowLbl=mn;
        float accW = 0.f;
                const char* fmtTag=".gdr";
        if(true){
            auto* eng3=GucciEngine::get();
            if(eng3->jaMacros.count(mn))fmtTag=".ja";
            else if(eng3->giddeyMacros.count(mn))fmtTag=".giddey";
            else if(eng3->toosiiMacros.count(mn))fmtTag=".toosii";
            else if(eng3->bamMacros.count(mn))fmtTag=".bam";
            else if(eng3->sexyyMacros.count(mn))fmtTag=".sexyy";
            else if(eng3->juiceMacros.count(mn))fmtTag=".juice";
            else if(eng3->butlerMacros.count(mn))fmtTag=".butler";
            else fmtTag=".brrr";}
        float fmtW=(!isIncompat)?(ImGui::CalcTextSize(fmtTag).x+8):0;
        float inW=isIncompat?(ImGui::CalcTextSize("Incompatible").x+8):0;
        float maxNW=std::max(40.f,fullW-rightReserved-accW-fmtW-inW-listPadX-8.f);
        if(ImGui::CalcTextSize(rowLbl.c_str()).x>maxNW){
            while(!rowLbl.empty()&&ImGui::CalcTextSize((rowLbl+"...").c_str()).x>maxNW)rowLbl.pop_back();
            rowLbl+="...";}
        ImGui::SetCursorPosX(ImGui::GetCursorPosX());
        ImVec2 rowStart=ImGui::GetCursorScreenPos();
        if(isIncompat)ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,0.4f));
                bool rowAct=ImGui::Selectable("##row",isSel,ImGuiSelectableFlags_AllowOverlap,ImVec2(fullW,rowH));
        bool rowDbl=ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(0);
        bool rowRC=ImGui::IsItemClicked(1);
        if(isIncompat)ImGui::PopStyleColor();
        if(rowDbl||rowRC){replayActionMacroName=mn;replayActionPopupRequested=true;}
        else if(!isIncompat&&rowAct&&!engine->isRecording()){

                        {
                                std::string extFound;
                auto dir = Mod::get()->getSaveDir()/"replays";
                for(auto& ext : {".brrr",".toosii",".ja",".giddey",".bam",".sexyy",".juice",".butler"}){
                    if(std::filesystem::exists(dir/(mn+ext))){extFound=ext;break;}
                }
                if(!extFound.empty()){
                    engine->replay.load(dir/(mn+extFound));
                    engine->replayName=mn;
                } else if(isIncompat){
                                        engine->convertToBRR(mn);
                    engine->replayName=mn;
                }
            }
        }
        float iy=rowStart.y,ih=rowH;
        auto* wdl=ImGui::GetWindowDrawList();
        wdl->AddText(ImVec2(rowStart.x+listPadX,iy+(ih-ImGui::GetTextLineHeight())*0.5f),
            isIncompat?toU32(ImVec4(1,1,1,0.4f)):theme.getTextU32(),rowLbl.c_str());
                float tagX=rowStart.x+fullW-rightReserved-4.f;
        if(isIncompat){
            auto ts=ImGui::CalcTextSize("Incompatible");tagX-=ts.x+4;
            wdl->AddText(ImVec2(tagX,iy+(ih-ts.y)*0.5f),toU32(ImVec4(1.f,0.2f,0.2f,1.f)),"Incompatible");}
        if(!isIncompat){
            const char* tag=".brrr";ImVec4 tagCol=getBRRTagColor();
            auto* eng2=GucciEngine::get();
            if(eng2->jaMacros.count(mn)){tag=".ja";tagCol=ImVec4(0.42f,0.78f,0.95f,1.f);}
            else if(eng2->giddeyMacros.count(mn)){tag=".giddey";tagCol=ImVec4(0.87f,0.12f,0.12f,1.f);}
            else if(eng2->toosiiMacros.count(mn)){tag=".toosii";tagCol=ImVec4(0.99f,0.82f,0.14f,1.f);}
            else if(eng2->bamMacros.count(mn)){tag=".bam";tagCol=ImVec4(0.878f,0.067f,0.153f,1.f);}
            else if(eng2->sexyyMacros.count(mn)){tag=".sexyy";tagCol=ImVec4(0.910f,0.004f,0.580f,1.f);}
            else if(eng2->juiceMacros.count(mn)){tag=".juice";tagCol=ImVec4(0.960f,0.520f,0.380f,1.f);}
            else if(eng2->butlerMacros.count(mn)){tag=".butler";tagCol=ImVec4(0.996f,0.725f,0.153f,1.f);}
            auto ts=ImGui::CalcTextSize(tag);tagX-=ts.x+4;
            wdl->AddText(ImVec2(tagX,iy+(ih-ts.y)*0.5f),toU32(tagCol),tag);}
                float btnY=iy+(ih-xBtnW)*0.5f;
        float xBtnX=rowStart.x+fullW-xBtnW-listPadX;
        float dotsBtnX=xBtnX-dotsBtnW-btnGap;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(0,0));
                ImGui::SetCursorScreenPos(ImVec2(dotsBtnX,btnY));
        if(ImGui::InvisibleButton("##dots",ImVec2(dotsBtnW,xBtnW))){
            replayActionMacroName=mn;replayActionPopupRequested=true;}
        bool dotsHov=ImGui::IsItemHovered();
        wdl->AddRectFilled(ImVec2(dotsBtnX,btnY),ImVec2(dotsBtnX+dotsBtnW,btnY+xBtnW),
            dotsHov?theme.getAccentU32(0.15f):IM_COL32(0,0,0,0),3.f);
        ImVec2 dts=ImGui::CalcTextSize("...");
        wdl->AddText(ImVec2(dotsBtnX+(dotsBtnW-dts.x)*0.5f,btnY+(xBtnW-dts.y)*0.5f),
            dotsHov?theme.getAccentU32():theme.getTextSecondaryU32(),"...");
                ImGui::SetCursorScreenPos(ImVec2(xBtnX,btnY));
        if(ImGui::InvisibleButton("##del",ImVec2(xBtnW,xBtnW))){
            auto dir=getReplayDir();
            for(auto& e:std::filesystem::directory_iterator(dir)){
                if(e.is_regular_file()&&e.path().stem().string()==mn){
                    std::filesystem::remove(e.path());break;}}
            markReplayListDirty();refreshReplayListIfNeeded(true);}
        bool delHov=ImGui::IsItemHovered();
        wdl->AddRectFilled(ImVec2(xBtnX,btnY),ImVec2(xBtnX+xBtnW,btnY+xBtnW),
            delHov?IM_COL32(200,50,50,60):IM_COL32(0,0,0,0),3.f);
        ImVec2 xts=ImGui::CalcTextSize("x");
        wdl->AddText(ImVec2(xBtnX+(xBtnW-xts.x)*0.5f,btnY+(xBtnW-xts.y)*0.5f),
            delHov?IM_COL32(255,100,100,255):theme.getTextSecondaryU32(),"x");
        ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);ImGui::PopStyleColor();
        if(replayActionPopupRequested){ImGui::OpenPopup("MacroActions");replayActionPopupRequested=false;}
    if(replayRenamePopupRequested){ImGui::OpenPopup("RenameReplay");replayRenamePopupRequested=false;}
    ImGui::SetNextWindowSize(ImVec2(260,0),ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0,0,0,0));
    if(ImGui::BeginPopupModal("MacroActions",nullptr,
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        drawPopupChrome(*this,"Macro Actions");
        ImGui::TextColored(theme.getAccent(),"%s",replayActionMacroName.c_str());
        ImGui::Dummy(ImVec2(0,6));
        float aw=230.f;
        if(Widgets::StyledButton("Rename##ar",ImVec2(aw,30),theme,anim,6.f)){
            replayRenameOriginalName=replayActionMacroName;
            strncpy(replayRenameBuffer,replayActionMacroName.c_str(),sizeof(replayRenameBuffer)-1);
            replayRenameError.clear();replayRenameFocusInput=true;
            ImGui::CloseCurrentPopup();replayRenamePopupRequested=true;}
        ImGui::Dummy(ImVec2(0,4));
                {bool canEdit_unused=true;
        if(false){
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
            Widgets::StyledButton("Open Frame Editor##ae",ImVec2(aw,30),theme,anim,6.f);
            ImGui::PopStyleVar();
            ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x,ImGui::GetItemRectMin().y+34),IM_COL32(255,180,80,200),"CBS/CBF macros cannot be edited");
        }else if(Widgets::StyledButton("Open Frame Editor##ae",ImVec2(aw,30),theme,anim,6.f)){
            BRRMacro* loaded=BRRMacro::loadFromDisk(replayActionMacroName);
            if(loaded){frameEditor.openBRR(replayActionMacroName,loaded);delete loaded;}
            ImGui::CloseCurrentPopup();}}

        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.55f,0.12f,0.12f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.75f,0.18f,0.18f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.4f,0.08f,0.08f,1.f));
        if(Widgets::StyledButton("Delete##ad",ImVec2(aw,30),theme,anim,6.f)){
            replayDeleteName=replayActionMacroName;replayDeleteError.clear();
            ImGui::CloseCurrentPopup();replayDeletePopupRequested=true;}
        ImGui::PopStyleColor(3);

        ImGui::Dummy(ImVec2(0,6));
        if(Widgets::StyledButton("Cancel##ac",ImVec2(aw,28),theme,anim,6.f))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();}
    ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
        if(replayDeletePopupRequested){ImGui::OpenPopup("DeleteReplay");replayDeletePopupRequested=false;}
    ImGui::SetNextWindowSize(ImVec2(300,0),ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0,0,0,0));
    if(ImGui::BeginPopupModal("DeleteReplay",nullptr,
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        drawPopupChrome(*this,"Delete Replay");
        ImGui::Text("Permanently delete:");
        ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f),"%s",replayDeleteName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("This removes the file from disk. It cannot be undone.");
        ImGui::PopStyleColor();
        if(!replayDeleteError.empty()){
            ImGui::Dummy(ImVec2(0,4));
            ImGui::TextColored(ImVec4(1.f,0.35f,0.35f,1.f),"%s",replayDeleteError.c_str());}
        ImGui::Dummy(ImVec2(0,10));
        float pbw=125.f;
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.55f,0.12f,0.12f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.75f,0.18f,0.18f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.4f,0.08f,0.08f,1.f));
        bool confirmDel=Widgets::StyledButton("Delete##cd",ImVec2(pbw,28),theme,anim,6.f);
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0,8);
        bool cancelDel=Widgets::StyledButton("Cancel##cd",ImVec2(pbw,28),theme,anim,6.f);
        if(confirmDel){
            if(deleteStoredReplay(replayDeleteName,replayDeleteError)){
                auto* eng4=GucciEngine::get();
                if(!eng4->isRecording()&&eng4->replayName==replayDeleteName){
                    eng4->replay.m_actionAtom.clear();
                    eng4->replayName.clear();}
                eng4->incompatibleMacros.erase(replayDeleteName);
                eng4->jaMacros.erase(replayDeleteName);eng4->giddeyMacros.erase(replayDeleteName);
                eng4->toosiiMacros.erase(replayDeleteName);eng4->bamMacros.erase(replayDeleteName);
                eng4->sexyyMacros.erase(replayDeleteName);eng4->juiceMacros.erase(replayDeleteName);
                eng4->butlerMacros.erase(replayDeleteName);
                replayDeleteName.clear();replayDeleteError.clear();
                markReplayListDirty();refreshReplayListIfNeeded(true);ImGui::CloseCurrentPopup();}}
        if(cancelDel){replayDeleteName.clear();replayDeleteError.clear();ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();}
    ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
        ImGui::SetNextWindowSize(ImVec2(320,0),ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0,0,0,0));
    if(ImGui::BeginPopupModal("RenameReplay",nullptr,
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        drawPopupChrome(*this,"Rename Replay");
        ImGui::Text("Rename replay:");
        ImGui::TextColored(theme.getAccent(),"%s",replayRenameOriginalName.c_str());
        ImGui::Dummy(ImVec2(0,6));
        if(replayRenameFocusInput){ImGui::SetKeyboardFocusHere();replayRenameFocusInput=false;}
        bool submitted=ImGui::InputText("##renameR",replayRenameBuffer,sizeof(replayRenameBuffer),
            ImGuiInputTextFlags_AutoSelectAll|ImGuiInputTextFlags_EnterReturnsTrue);
        if(!replayRenameError.empty()){
            ImGui::Dummy(ImVec2(0,4));
            ImGui::TextColored(ImVec4(1.f,0.35f,0.35f,1.f),"%s",replayRenameError.c_str());}
        ImGui::Dummy(ImVec2(0,10));
        float pbw=110.f;
        bool confirm=Widgets::StyledButton("Rename##cr",ImVec2(pbw,28),theme,anim,6.f);
        ImGui::SameLine(0,8);
        bool cancel=Widgets::StyledButton("Cancel##cr",ImVec2(pbw,28),theme,anim,6.f);
        if(confirm||submitted){
            std::string renamedTo;
            if(renameStoredReplay(replayRenameOriginalName,replayRenameBuffer,renamedTo,replayRenameError)){
                auto* engine2=GucciEngine::get();
                if(!engine2->isRecording()&&engine2->replayName==replayRenameOriginalName)
                    engine2->replayName=renamedTo;
                replayRenameOriginalName.clear();replayRenameError.clear();replayRenameBuffer[0]=0;
                markReplayListDirty();refreshReplayListIfNeeded(true);ImGui::CloseCurrentPopup();
            }
        }
        if(cancel){replayRenameOriginalName.clear();replayRenameError.clear();replayRenameBuffer[0]=0;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();}
    ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0,4));
    float bw=(ImGui::GetContentRegionAvail().x-10)/2.f;
    if(Widgets::StyledButton("Refresh",ImVec2(bw,28),theme,anim)){markReplayListDirty();refreshReplayListIfNeeded(true);}
    ImGui::SameLine(0,10);
    if(Widgets::StyledButton("Open Folder",ImVec2(bw,28),theme,anim)){
        auto dir=getReplayDir();
        if(std::filesystem::exists(dir)||std::filesystem::create_directory(dir))utils::file::openFolder(dir);}

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Macro Diff",theme);
    static char diffNameA[128] = {}, diffNameB[128] = {};
    static std::vector<GucciEngine::DiffEntry> diffResults;
    static bool diffRun = false;
    ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - 10) / 2.f);
    ImGui::InputText("##diffA", diffNameA, sizeof(diffNameA));
    ImGui::SameLine(0,10);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##diffB", diffNameB, sizeof(diffNameB));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
    ImGui::TextUnformatted("A                                          B");
    ImGui::PopStyleColor();
    if(Widgets::StyledButton("Compare", ImVec2(-1, 28), theme, anim) && diffNameA[0] && diffNameB[0]){
        diffResults = engine->diffMacros(diffNameA, diffNameB);
        diffRun = true;
    }
    if(diffRun){
        if(diffResults.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f,1,0.3f,1));
            ImGui::TextUnformatted("No differences found.");
            ImGui::PopStyleColor();
        } else {
            char lbl[64]; snprintf(lbl, sizeof(lbl), "%zu difference(s):", diffResults.size());
            ImGui::TextUnformatted(lbl);
            float listH = std::min((float)diffResults.size() * 18.f, 180.f);
            ImGui::BeginChild("##diffList", ImVec2(-1, listH), true);
            for(auto& d : diffResults){
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextUnformatted(d.description.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }
    }

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Macro Surgery",theme);
    static char trimName[128]={}; static int trimStart=0,trimEnd=0; static bool trimRebase=true;
    static int mergeGap=0; static std::string surgeryStatus;
    float sgw=ImGui::GetContentRegionAvail().x;
    ImGui::Text("Trim");ImGui::SameLine(70);ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##trimN","macro name",trimName,sizeof(trimName));
    ImGui::Text("Range");ImGui::SameLine(70);
    ImGui::SetNextItemWidth((sgw-70-8)*0.5f);
    ImGui::InputInt("##trimS",&trimStart,0,0);
    ImGui::SameLine(0,8);ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##trimE",&trimEnd,0,0);
    Widgets::ToggleSwitch("Rebase to frame 0",&trimRebase,theme,anim);
    if(Widgets::StyledButton("Trim -> _trim",ImVec2(-1,26),theme,anim)&&trimName[0]){
        surgeryStatus=engine->trimMacro(trimName,trimStart,trimEnd,trimRebase)
            ?"Trim saved.":"Trim failed (check name and range).";
        markReplayListDirty();refreshReplayListIfNeeded(true);
    }
    ImGui::Dummy(ImVec2(0,6));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Merge appends B after A using the Compare fields above. Gap = extra frames between them.");
    ImGui::PopStyleColor();
    ImGui::Text("Gap");ImGui::SameLine(70);ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##mGap",&mergeGap,0,0);
    if(Widgets::StyledButton("Merge A + B -> _merged",ImVec2(-1,26),theme,anim)&&diffNameA[0]&&diffNameB[0]){
        surgeryStatus=engine->mergeMacros(diffNameA,diffNameB,mergeGap)
            ?"Merge saved.":"Merge failed (check the Compare names).";
        markReplayListDirty();refreshReplayListIfNeeded(true);
    }
    if(!surgeryStatus.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextUnformatted(surgeryStatus.c_str());
        ImGui::PopStyleColor();
    }

        if(!engine->incompatibleMacros.empty()){
        ImGui::Dummy(ImVec2(0,8));
        Widgets::SectionHeader("Convert to BRR",theme);
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("These macros are in legacy format. Click to convert to BRR.");
        ImGui::PopStyleColor();
        for(auto& mn : engine->incompatibleMacros){
            if(Widgets::StyledButton(("Convert: "+mn).c_str(),ImVec2(-1,26),theme,anim)){
                engine->convertToBRR(mn);
                markReplayListDirty();refreshReplayListIfNeeded(true);
            }
        }
    }
        if(!engine->replay.m_actionAtom.m_actions.empty()&&engine->isPlaying()&&PlayLayer::get()){
        size_t cnt=engine->replay.m_actionAtom.m_actions.size();
        std::string nm=engine->replayName;
        ImGui::Dummy(ImVec2(0,4));Widgets::SectionHeader("Loaded Macro",theme);
        ImGui::Text("Name: %s",nm.c_str());
        ImGui::SameLine();ImGui::PushStyleColor(ImGuiCol_Text,getBRRTagColor());ImGui::TextUnformatted("(BRR)");ImGui::PopStyleColor();
        ImGui::Text("Actions: %zu",cnt);
                {
            std::string lvl=engine->loadedMacroLevelName;
            if(!lvl.empty()){ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::Text("Level: %s",lvl.c_str());ImGui::PopStyleColor();}
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::Text("TPS: %d",(int)engine->updater.m_tps);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0,4));
        Widgets::ToggleSwitch("Ignore Manual Input",&engine->replay.m_ignoreInputs,theme,anim);
        if(engine->replay.m_pathSamples.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("No path data for this macro -- only recorded going forward, not backfilled for older macros.");
            ImGui::PopStyleColor();
        } else if(Widgets::ToggleSwitch("Show Macro Path",&engine->showMacroPath,theme,anim)){
            Mod::get()->setSavedValue("hack_show_macro_path",engine->showMacroPath);
        }
        if(engine->showMacroPath&&!engine->replay.m_pathSamples.empty()){
            if(Widgets::StyledSliderFloat("Marker Size",&engine->macroPathMarkerSize,3.f,20.f,theme))
                Mod::get()->setSavedValue("hack_macro_path_marker_size",(double)engine->macroPathMarkerSize);
            if(Widgets::StyledSliderFloat("Line Opacity",&engine->macroPathLineOpacity,0.1f,1.f,theme))
                Mod::get()->setSavedValue("hack_macro_path_line_opacity",(double)engine->macroPathLineOpacity);
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Line shows the recorded path. Squares mark clicks (and releases in Wave/Robot/Ship) -- drawn with an inverted-colour blend so they stay visible over any terrain.");
            ImGui::PopStyleColor();
        }
    }
}

void MenuInterface::drawToolsTab(){
    auto* engine=GucciEngine::get();
    if((activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE))
        Widgets::GucciQuote("\"I run the route so fast the DB thinks I'm a speedhack.\"","-- Toosii, route running",theme);
    else if(activeTheme==THEME_JA)
        Widgets::GucciQuote("\"I don't use speedhack. That's just me.\""," -- Ja Morant",theme);
    else if(activeTheme==THEME_GIDDEY)
        Widgets::GucciQuote("\"Speed 1.0x seems fast enough. I'm still jetlagged.\""," -- Josh Giddey",theme);
    else if(activeTheme==THEME_BAM)
        Widgets::GucciQuote("\"Speed? I hit 83 at my own pace. You can't guard that.\"","-- Bam, on speedhack",theme);
    else if(activeTheme==THEME_SEXYY)
        Widgets::GucciQuote("\"I run this at my own speed and it still goes stupid.\"","-- Sexyy Red",theme);
    else if(activeTheme==THEME_JUICE)
        Widgets::GucciQuote("\"Speed doesn't mean much if the frame windows are wrong.\"","-- Juice, keeping you honest",theme);
    else if(activeTheme==THEME_BUTLER)
        Widgets::GucciQuote("\"I don't need speedhack. I just lock in.\"","-- Jimmy Butler",theme);
    else
        Widgets::GucciQuote("\"I run this game at my own speed. You can't keep up.\"","-- Gucci Mane, on speedhacks",theme);
    Widgets::SectionHeader("TPS Control",theme);
    ImGui::TextColored(theme.getAccent(),"Current: %.0f TPS",engine->updater.m_tps);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-90);
    ImGui::InputFloat("##tps",&tempTickRate,0,0,"%.0f");
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Apply###tps",ImVec2(78,28),theme,anim)){
        bool can=!PlayLayer::get()||!engine->isPlaying();
        if(can){engine->updater.m_tps=tempTickRate;Mod::get()->setSavedValue("eng_tick_rate",(float)engine->updater.m_tps);}}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Speed Control",theme);
    ImGui::TextColored(theme.getAccent(),"Current: %.2fx",engine->updater.m_speedhack);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-90);
    ImGui::InputFloat("##spd",&tempGameSpeed,0,0,"%.2f");
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Apply###spd",ImVec2(78,28),theme,anim))engine->updater.m_speedhack=tempGameSpeed;
    ImGui::Dummy(ImVec2(0,12));
    Widgets::SectionHeader("Features",theme);
    if(Widgets::ModuleCardBegin("Frame Advance",
        (activeTheme==THEME_TOOSII)?"Frame advance, like a running back hitting the hole":"Pause and step frame-by-frame",
        &engine->updater.m_paused,theme,anim,&keybinds.frameAdvance))Widgets::ModuleCardEnd();
    if(Widgets::ModuleCardBegin("Speedhack Audio","Apply speed changes to game audio",&engine->audioPitchEnabled,theme,anim,&keybinds.audioPitch))
        Widgets::ModuleCardEnd();
    if(Widgets::ModuleCardBegin("Layout Mode","Remove all decorations",&engine->layoutMode,theme,anim,&keybinds.layoutMode))
        Widgets::ModuleCardEnd();
    if(Widgets::ModuleCardBegin("No Mirror Effect",
        (activeTheme==THEME_TOOSII)?"Can't tackle what you can't see. Block that mirror.":"Disable mirror portal visual flip",
        &engine->noMirrorEffect,theme,anim,&keybinds.noMirror)){
        Widgets::ToggleSwitch("Only Recording",&engine->noMirrorRecordingOnly,theme,anim);
        Widgets::ModuleCardEnd();}

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("v4.0 — From Silicate",theme);

    if(Widgets::ModuleCardBegin("Backwards Stepping",
        "Step backwards through recorded frames during frame advance",
        &engine->updater.m_backwardsStepping,theme,anim,&keybinds.backStep)){
        Widgets::StyledSliderInt("History Size",reinterpret_cast<int*>(&engine->updater.m_maxBackstepFrames),10,480,theme);
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Stores up to N ticks for rewind. Press Back Step hotkey while frame-advancing.");
        ImGui::PopStyleColor();
        Widgets::ModuleCardEnd();}

    if(Widgets::ModuleCardBegin("Lock Delta",
        "Lock physics dt for deterministic simulation",
        &engine->updater.m_lockDelta,theme,anim)){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Forces exact 1/TPS every step. The old Performance mode (batching multiple ticks into one to catch up) was removed -- it measurably undercounted the frame number relative to real physics progress, and this bot doesn't need the speed badly enough to be worth that.");
        ImGui::PopStyleColor();
        Widgets::ModuleCardEnd();}

    if(Widgets::ModuleCard("Frame Extrapolation",
        "Smoothly interpolate player position between physics ticks",
        &engine->updater.m_extrapolateFrames,theme,anim)){}

        ImGui::Dummy(ImVec2(0,4));
    Widgets::ToggleSwitch("Show Noclip Accuracy",&engine->noclipAccuracyVisible,theme,anim);

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Autosave",theme);
    if(Widgets::ToggleSwitch("Save on Level Complete",&engine->autosaveAtLevelEnd,theme,anim))
        Mod::get()->setSavedValue("autosave_atLevelEnd",engine->autosaveAtLevelEnd);
    if(Widgets::ToggleSwitch("Save at Interval",&engine->autosaveAtInterval,theme,anim)){
        Mod::get()->setSavedValue("autosave_atInterval",engine->autosaveAtInterval);
        engine->applyIntervalAutosave();
    }
    if(engine->autosaveAtInterval){
        float intervalF=static_cast<float>(engine->autosaveIntervalSec);
        if(Widgets::StyledSliderFloat("Interval (sec)",&intervalF,10.f,600.f,theme)){
            engine->autosaveIntervalSec=static_cast<double>(intervalF);
            Mod::get()->setSavedValue("autosave_interval",engine->autosaveIntervalSec);
            engine->applyIntervalAutosave();
        }
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        char ibuf[32];snprintf(ibuf,sizeof(ibuf),"%.0f sec",engine->autosaveIntervalSec);
        ImGui::Text("Saves every %s while recording.",ibuf);
        ImGui::PopStyleColor();}
    Widgets::ToggleSwitch("Backup Before Overwrite",&engine->replayBackupsEnabled,theme,anim);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Backups saved to replays/backups/ subfolder.");
    ImGui::PopStyleColor();}

void MenuInterface::drawHacksTab(){
    auto* engine=GucciEngine::get();
    if((activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE))
        Widgets::GucciQuote("\"I'm open every play. They just never throw me the ball.\"","-- Toosii, open in the end zone",theme);
    else if(activeTheme==THEME_JA)
        Widgets::GucciQuote("\"Noclip is just confidence. I go where I want.\""," -- Ja Morant",theme);
    else if(activeTheme==THEME_GIDDEY)
        Widgets::GucciQuote("\"I flew 17 hours to play this game. Where's the replay button?\"","-- Josh Giddey, probably",theme);
    else if(activeTheme==THEME_BAM)
        Widgets::GucciQuote("\"Noclip? I don't need it. The rim can't stop me either.\"","-- Bam, probably",theme);
    else if(activeTheme==THEME_SEXYY)
        Widgets::GucciQuote("\"Noclip? Baby I walk through walls naturally.\"","-- Sexyy Red",theme);
    else if(activeTheme==THEME_JUICE)
        Widgets::GucciQuote("\"Noclip's cool. I just want the analyzer to work.\"","-- Juice, still testing",theme);
    else if(activeTheme==THEME_BUTLER)
        Widgets::GucciQuote("\"Noclip? I go through everything. That's just Playoff Jimmy.\"","-- Jimmy Butler",theme);
    else
        Widgets::GucciQuote("\"I never die in this game. I'm immune. Like Gucci flu.\"","-- GucciBot propaganda",theme);
    ImGui::Dummy(ImVec2(0,4));
    if(Widgets::ModuleCardBegin("Safe Mode",
        (activeTheme==THEME_TOOSII)?"Safe mode is just playing with no pads. Still catching everything.":"Prevents stats and percentage gain",
        &engine->protectedMode,theme,anim,&keybinds.safeMode))Widgets::ModuleCardEnd();
    if(Widgets::ModuleCardBegin("Show Trajectory",
        (activeTheme==THEME_TOOSII)?"Run the route. Don't look back. Ball's already there.":"Display predicted player path",
        &engine->pathPreview,theme,anim,&keybinds.trajectory)){
        Widgets::StyledSliderInt("Trajectory Length",&engine->pathLength,50,480,theme);
        Widgets::ModuleCardEnd();}
    if(Widgets::ModuleCardBegin("Show Hitboxes","Display collision bounds for objects",&engine->showHitboxes,theme,anim,&keybinds.hitboxes)){
        Widgets::ToggleSwitch("On Death Only",&engine->hitboxOnDeath,theme,anim);
        Widgets::ToggleSwitch("Draw Trail",&engine->hitboxTrail,theme,anim);
        if(engine->hitboxTrail)Widgets::StyledSliderInt("Trail Length",&engine->hitboxTrailLength,10,600,theme);
        Widgets::ModuleCardEnd();}
    if(Widgets::ModuleCardBegin("Noclip",
        (activeTheme==THEME_TOOSII)?"Can't cover what you can't see. Route so clean it's invisible.":"Disable collision with obstacles",
        &engine->noclipEnabled,theme,anim,&keybinds.noclip)){

                if(engine->noclipAccuracyVisible){
            float pct = engine->noclipAccuracy * 100.f;
            ImVec4 hc = pct>=90?ImVec4(0.3f,1,0.3f,1):pct>=70?ImVec4(1,1,0.3f,1):ImVec4(1,0.3f,0.3f,1);
            ImGui::Text("Accuracy: ");ImGui::SameLine();ImGui::TextColored(hc,"%.2f%%",pct);
            ImGui::Dummy(ImVec2(0,4));
            bool hasThresh = engine->noclipThreshold > 0.f;
            if(Widgets::ToggleSwitch("Accuracy Threshold",&hasThresh,theme,anim))
                engine->noclipThreshold = hasThresh ? 0.80f : 0.f;
            if(hasThresh){
                ImGui::SetNextItemWidth(-1);
                float t = engine->noclipThreshold * 100.f;
                if(ImGui::SliderFloat("##noclipThresh",&t,1.f,100.f,"%.1f%%"))
                    engine->noclipThreshold = t / 100.f;
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::TextWrapped("Noclip disables itself when accuracy reaches this value.");
                ImGui::PopStyleColor();}
            ImGui::Dummy(ImVec2(0,4));}

        Widgets::ToggleSwitch("On Death Color",&engine->noclipDeathFlash,theme,anim);
        if(engine->noclipDeathFlash){
            float col[3]={engine->noclipDeathColorR,engine->noclipDeathColorG,engine->noclipDeathColorB};
            if(ImGui::ColorEdit3("##dc",col,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel)){
                engine->noclipDeathColorR=col[0];engine->noclipDeathColorG=col[1];engine->noclipDeathColorB=col[2];}}
        Widgets::ModuleCardEnd();}

    if(Widgets::ModuleCardBegin("RNG Lock",
        (activeTheme==THEME_TOOSII)?"Fixed seed. Like my routes -- always finding the soft spot in zone.":"Use fixed seed for consistent RNG",
        &engine->rngLocked,theme,anim,&keybinds.rngLock)){
        if(!rngBufferInit){snprintf(rngBuffer,sizeof(rngBuffer),"%u",engine->rngSeedVal);rngBufferInit=true;}
        ImGui::Text("Seed Value:");ImGui::SetNextItemWidth(-1);
        if(ImGui::InputText("##seed",rngBuffer,sizeof(rngBuffer),ImGuiInputTextFlags_CharsDecimal)){
            try{engine->rngSeedVal=(unsigned)std::stoull(rngBuffer);}catch(...){engine->rngSeedVal=1;}}
        Widgets::ModuleCardEnd();}

        ImGui::Dummy(ImVec2(0,4));
    Widgets::SectionHeader("v4.0 — From Silicate",theme);

    if(Widgets::ModuleCardBegin("Auto-Flip on Death",
        "Flip gravity instead of dying -- great for mirror levels",
        &engine->updater.m_autoFlipOnDeath,theme,anim,&keybinds.autoFlip)){
        if(engine->updater.m_isAutoFlipped){
            Widgets::StatusBadge("FLIPPED",ImVec4(0.4f,0.8f,1.f,1.f));}
        Widgets::ModuleCardEnd();}

    if(Widgets::ModuleCard("Prevent Death",
        "Absorb all hits silently -- no collision counter",
        &engine->updater.m_preventDeath,theme,anim,&keybinds.preventDeath)){}

    if(Widgets::ModuleCardBegin("Mirror Inputs",
        "Replay as if left/right controls are swapped",
        &engine->replay.m_mirrorInputs,theme,anim,&keybinds.mirrorInputs)){
        Widgets::ToggleSwitch("Invert Players",&engine->replay.m_mirrorInverted,theme,anim);
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Invert Players: swap which player each input goes to.");
        ImGui::PopStyleColor();
        Widgets::ModuleCardEnd();}

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Frame Window Tracker",theme);
    if(Widgets::ToggleSwitch("Show Live",&engine->fwEnabledLive,theme,anim))
        Mod::get()->setSavedValue("fw_live",engine->fwEnabledLive);
    if(Widgets::ToggleSwitch("Show in Renders",&engine->fwEnabledRender,theme,anim))
        Mod::get()->setSavedValue("fw_render",engine->fwEnabledRender);
    if(Widgets::ToggleSwitch("Show Legend",&engine->fwLegendEnabled,theme,anim))
        Mod::get()->setSavedValue("fw_legend",engine->fwLegendEnabled);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Juice's idea: a running tally in the top-left corner, like the frame-window counter overlays in some GD YouTube videos -- how many of the clicks reached so far landed in each Tier's window range below. Shows nothing until at least one Tier is configured and Calculate has results.");
    ImGui::PopStyleColor();
    if(Widgets::ToggleSwitch("Test Ship Releases",&engine->fwTestShipReleases,theme,anim))
        Mod::get()->setSavedValue("fw_test_ship_releases",engine->fwTestShipReleases);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Calculate measures release timing windows too now, for Wave/Ship/Robot (the only gamemodes where a release's timing matters) -- Ship's can be finicky to probe reliably, so it has its own switch here.");
    ImGui::PopStyleColor();
    if(Widgets::ToggleSwitch("Orb-Aware Release Skip",&engine->fwOrbAwareReleaseSkip,theme,anim))
        Mod::get()->setSavedValue("fw_orb_aware_release_skip",engine->fwOrbAwareReleaseSkip);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("In Robot mode, a release right after clicking a non-dash orb isn't treated as its own measurable input (dash orbs and non-orb clicks still are). Turn off to go back to testing every Robot release, no exceptions.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Practice Range",theme);
    if(Widgets::ToggleSwitch("Show During Playback",&engine->practiceRangeEnabled,theme,anim))
        Mod::get()->setSavedValue("practice_range",engine->practiceRangeEnabled);
            if(engine->fwMaxWindow > 2*engine->fwSweepRange){
        engine->fwMaxWindow = 2*engine->fwSweepRange;
        Mod::get()->setSavedValue("fw_maxwindow",(int64_t)engine->fwMaxWindow);
    }
    if(Widgets::StyledSliderInt("Max Window (frames)",&engine->fwMaxWindow,1,2*engine->fwSweepRange,theme))
        Mod::get()->setSavedValue("fw_maxwindow",(int64_t)engine->fwMaxWindow);
    if(Widgets::StyledSliderInt("Sweep Range (+/- frames)",&engine->fwSweepRange,1,30,theme))
        Mod::get()->setSavedValue("fw_sweeprange",(int64_t)engine->fwSweepRange);
    if(Widgets::StyledSliderInt("Slack Window (+/- frames)",&engine->fwSlackWindow,0,20,theme))
        Mod::get()->setSavedValue("fw_slackwindow",(int64_t)engine->fwSlackWindow);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("A shift survives if it stays alive through roughly how long the original macro takes to reach the next input, plus or minus this many frames of slack. The next input itself is never moved -- it always fires at its own original frame.");
    ImGui::PopStyleColor();
    if(Widgets::ToggleSwitch("Full-Range Sweep",&engine->fwFullRangeSweep,theme,anim))
        Mod::get()->setSavedValue("fw_full_range_sweep",engine->fwFullRangeSweep);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Off (default): stop expanding a direction the moment one shift fails there. On: keep testing every shift out to the sweep range regardless of failures in between, so non-contiguous survivable windows actually show up instead of being silently missed. Slower.");
    ImGui::PopStyleColor();
    if(Widgets::StyledSliderInt("Max Frames Measured",&engine->fwMaxFramesMeasured,16,480,theme))
        Mod::get()->setSavedValue("fw_maxframes",(int64_t)engine->fwMaxFramesMeasured);
    if(Widgets::StyledSliderInt("Simulation Speed",&engine->fwSimSpeed,1,8,theme))
        Mod::get()->setSavedValue("fw_simspeed",(int64_t)engine->fwSimSpeed);
    if(Widgets::ToggleSwitch("Debug Mode",&engine->fwDebugMode,theme,anim))
        Mod::get()->setSavedValue("fw_debug_mode",engine->fwDebugMode);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Pauses briefly after every individual shift test and drops a green/red mark where the player ended up, so you can watch Calculate work through a click instead of only seeing the final number.");
    ImGui::PopStyleColor();
    if(Widgets::ToggleSwitch("Delay Marker Capture (diagnostic)",&engine->fwDelayMarkerCapture,theme,anim))
        Mod::get()->setSavedValue("fw_delay_marker_capture",engine->fwDelayMarkerCapture);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Juice's position-lag report: run Calculate once with this off, once with it on, on the same macro/click. Doesn't change any measurement, only where the marker ring gets drawn -- whichever run's rings actually line up with the real click tells us which way the fix needs to go.");
    ImGui::PopStyleColor();
    if(engine->fwDebugMode && Widgets::StyledSliderInt("Debug Pause (ticks)",&engine->fwDebugSlowdown,1,120,theme))
        Mod::get()->setSavedValue("fw_debug_slowdown",(int64_t)engine->fwDebugSlowdown);
    if(!engine->fwDebugMarks.empty() && Widgets::StyledButton("View Debug History (...)",ImVec2(-1,28),theme,anim,6.f))
        ImGui::OpenPopup("FwDebugHistory");
    ImGui::SetNextWindowSize(ImVec2(440,0),ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14,12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0,0,0,0));
    if(ImGui::BeginPopupModal("FwDebugHistory",nullptr,
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        drawPopupChrome(*this,"Debug History");
        ImGui::TextColored(theme.getAccent(),"%zu test(s) recorded this run",engine->fwDebugMarks.size());
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("\"Go\" teleports you to that test's exact checkpoint + shift and lets it play out exactly like the real test did -- watch it, or take over yourself.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0,6));
        float listH=std::min((float)engine->fwDebugMarks.size()*26.f,320.f);
        ImGui::BeginChild("##fwDebugHistList",ImVec2(410,listH),true);
        for(size_t i=0;i<engine->fwDebugMarks.size();++i){
            auto const& mk=engine->fwDebugMarks[i];
            ImGui::PushID((int)i+11000);
            ImGui::TextColored(mk.survived?ImVec4(0.3f,1.f,0.4f,1.f):ImVec4(1.f,0.3f,0.3f,1.f),
                "%s",mk.survived?"PASS":"FAIL");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::Text("in#%d f=%u -> shift f=%u  %s p%d",
                mk.inputNumber,mk.macroFrame,mk.testedFrame,mk.isRelease?"rel":"press",mk.player2?2:1);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if(Widgets::StyledButton("Go",ImVec2(40,20),theme,anim,4.f)){
                engine->debugTeleportToMark(i);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0,6));
        if(Widgets::StyledButton("Close##fwDebugHist",ImVec2(-1,28),theme,anim,6.f))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);ImGui::PopStyleVar(2);
            if(engine->fwAnalyzeRunning){
        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,theme.getAccent());
        char ov[64];
        if(engine->fwAnalyzeTotal>0)
            snprintf(ov,sizeof(ov),"%s  %d/%d  (%.0f%%)",
                engine->fwAnalyzeStage.c_str(),engine->fwAnalyzeCur,
                engine->fwAnalyzeTotal,engine->fwAnalyzeProgress*100.f);
        else
            snprintf(ov,sizeof(ov),"%s  (%.0f%%)",
                engine->fwAnalyzeStage.c_str(),engine->fwAnalyzeProgress*100.f);
        ImGui::ProgressBar(engine->fwAnalyzeProgress,ImVec2(-1,18),ov);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    if(engine->fwHasData){
        int vis=0,loosest=0;
        for(auto const&mk:engine->fwMarks){ if(mk.window<=engine->fwMaxWindow)++vis; if(mk.window>loosest)loosest=mk.window; }
        ImGui::Text("%zu clicks analyzed, %d shown (window <= %d).",
            engine->fwMarks.size(),vis,engine->fwMaxWindow);
        if(vis==0 && !engine->fwMarks.empty())
            ImGui::TextWrapped("None visible: every click is looser than %d frames (loosest is %d). Raise Max Window to see them.",
                engine->fwMaxWindow,loosest);
    }
    else if(!engine->fwAnalyzeRunning)
        ImGui::TextWrapped("No analysis yet. Save a macro while in the level and choose Calculate to simulate frame windows.");
    ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Manual Frame Windows",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Set or override a click's window by hand -- for clicks Calculate hasn't measured yet, or a reading you don't trust. Manual entries are protected: re-running Calculate fills in everything else but leaves these alone.");
    ImGui::PopStyleColor();
    if(engine->fwAnalyzing){
        // Juice's GUI-overlap report: this list used to read engine->replay.m_actionAtom
        // live, every GUI frame -- but that's the exact same atom Calculate's probing
        // continuously reassigns/reshapes (filtered to inputs-only, shifted, resorted)
        // many times per second while measuring. Rendering it live during Calculate meant
        // showing a different, transient, probe-internal snapshot practically every frame
        // instead of a stable view of the real macro -- which is what "compressed/
        // overlapping" almost certainly was. Just don't render the interactive list at all
        // while Calculate is running, instead of reading data that isn't meant to be read
        // from here right now.
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Unavailable while Calculate is running -- it's actively reshaping the macro's action list to run its tests. Manual Frame Windows will show up again once it finishes.");
        ImGui::PopStyleColor();
    } else {
        auto& acts = engine->replay.m_actionAtom.m_actions;
        auto& samples = engine->replay.m_pathSamples;
        std::vector<size_t> clickIdx;
        for(size_t i=0;i<acts.size();++i) if(acts[i].isInput()) clickIdx.push_back(i);

        if(clickIdx.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("No macro loaded, or it has no inputs to list.");
            ImGui::PopStyleColor();
        } else {
            float listH=std::min((float)clickIdx.size()*24.f,200.f);
            ImGui::BeginChild("##fwManualList",ImVec2(-1,listH),true);
            for(size_t row=0; row<clickIdx.size(); ++row){
                auto& a = acts[clickIdx[row]];
                ImGui::PushID((int)row+9000);

                GucciEngine::FrameWindowMark* mk=nullptr;
                for(auto& m:engine->fwMarks)
                    if(m.frame==a.m_frame && m.player2==a.m_player2){ mk=&m; break; }

                float pct = mk ? mk->percent : -1.f;
                if(!mk && a.m_frame<samples.size() && engine->m_levelLength>0.f){
                    float px = a.m_player2 ? samples[a.m_frame].p2x : samples[a.m_frame].p1x;
                    pct = std::clamp(px/engine->m_levelLength*100.f,0.f,100.f);
                }

                bool isRel=!a.m_holding;
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                if(pct>=0.f) ImGui::Text("f=%u  p%d  %s  %.1f%%",a.m_frame,a.m_player2?2:1,isRel?"rel":"press",pct);
                else ImGui::Text("f=%u  p%d  %s",a.m_frame,a.m_player2?2:1,isRel?"rel":"press");
                ImGui::PopStyleColor();
                ImGui::SameLine(190);

                int win = mk?mk->window:0;
                ImGui::SetNextItemWidth(60);
                if(ImGui::InputInt("##win",&win,0,0)){
                    win=std::max(1,win);
                    if(mk){ mk->window=win; mk->manual=true; }
                    else {
                        GucciEngine::FrameWindowMark nm;
                        nm.frame=a.m_frame; nm.player2=a.m_player2; nm.window=win; nm.manual=true;
                        nm.isRelease=isRel;
                        if(a.m_frame<samples.size()){
                            nm.x = a.m_player2?samples[a.m_frame].p2x:samples[a.m_frame].p1x;
                            nm.y = a.m_player2?samples[a.m_frame].p2y:samples[a.m_frame].p1y;
                        }
                        nm.percent = pct>=0.f?pct:0.f;
                        engine->fwMarks.push_back(nm);
                        engine->fwHasData=true;
                    }
                }
                ImGui::SameLine();
                if(mk){
                    ImGui::PushStyleColor(ImGuiCol_Text, mk->manual?theme.getAccent():theme.textSecondary);
                    ImGui::TextUnformatted(mk->manual?"manual":"auto");
                    ImGui::PopStyleColor();
                    if(mk->manual){
                        ImGui::SameLine();
                        if(Widgets::StyledButton("Clear",ImVec2(50,20),theme,anim,4.f)){
                            engine->fwMarks.erase(engine->fwMarks.begin()+(mk-engine->fwMarks.data()));
                            engine->fwHasData=!engine->fwMarks.empty();
                        }
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            if(Widgets::StyledButton("Save Manual Marks",ImVec2(-1,26),theme,anim,6.f))
                engine->saveFwMarksNow();
        }
    }

        ImGui::Dummy(ImVec2(0,6));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Tiers map gap sizes to a marker image and sound. Put PNG/audio files in the mod's fw_assets folder and enter the filenames. No tier = default colored ring.");
    ImGui::TextWrapped("Sound: leave blank for the built-in default chime, type a filename for custom, or 'none' to silence that tier. Markers appear as the bot reaches each click.");
    ImGui::PopStyleColor();
    int tierRemove=-1;
    for(size_t ti=0;ti<engine->fwTiers.size();++ti){
        auto& t=engine->fwTiers[ti];
        ImGui::PushID((int)(7000+ti));
        ImGui::Separator();
        float third=(ImGui::GetContentRegionAvail().x-16)/3.f;
        ImGui::SetNextItemWidth(third);
        ImGui::InputInt("##lo",&t.lo,0,0); ImGui::SameLine(0,8);
        ImGui::SetNextItemWidth(third);
        ImGui::InputInt("##hi",&t.hi,0,0); ImGui::SameLine(0,8);
        if(Widgets::StyledButton("X",ImVec2(-1,22),theme,anim,4.f))tierRemove=(int)ti;
        ImGui::Text("Range %d-%d frames",t.lo,t.hi);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##img","marker.png (in fw_assets)",t.imageFile,sizeof(t.imageFile));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##snd","sound file, or 'none' for silent",t.soundFile,sizeof(t.soundFile));
        float col3[3]={t.r,t.g,t.b};
        if(ImGui::ColorEdit3("##col",col3,ImGuiColorEditFlags_NoInputs)){
            t.r=col3[0];t.g=col3[1];t.b=col3[2];
        }
        ImGui::SameLine();ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextUnformatted("tint / ring color");ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if(tierRemove>=0)engine->fwTiers.erase(engine->fwTiers.begin()+tierRemove);
    ImGui::Separator();
    if(Widgets::StyledButton("+ Add Range",ImVec2(-1,26),theme,anim,6.f)){
        GucciEngine::FrameWindowTier nt;
        if(!engine->fwTiers.empty()){nt.lo=engine->fwTiers.back().hi+1;nt.hi=nt.lo+3;}
        engine->fwTiers.push_back(nt);
    }
    if(!engine->fwTiers.empty()){
        if(Widgets::StyledButton("Save Tiers",ImVec2(-1,24),theme,anim,6.f)){
            std::string enc;
            for(auto& t:engine->fwTiers){
                enc+=std::to_string(t.lo)+"|"+std::to_string(t.hi)+"|"+
                     t.imageFile+"|"+t.soundFile+"|"+
                     std::to_string(t.r)+"|"+std::to_string(t.g)+"|"+std::to_string(t.b)+";";
            }
            Mod::get()->setSavedValue("fw_tiers",enc);
        }
    }

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Frame Stepping",theme);
    auto& upd=engine->updater;
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Current frame: %u",upd.getFrame());
    ImGui::PopStyleColor();
    bool paused=upd.m_paused;
    if(Widgets::ToggleSwitch("Pause Physics",&paused,theme,anim))upd.setPaused(paused);
    if(upd.m_paused){
        float bw=(ImGui::GetContentRegionAvail().x-8)/2.f;
        if(Widgets::StyledButton("<< Step Back",ImVec2(bw,28),theme,anim,6.f)){
            if(upd.m_backwardsStepping)upd.backwardsStep(1);
        }
        ImGui::SameLine(0,8);
        if(Widgets::StyledButton("Step Fwd >>",ImVec2(bw,28),theme,anim,6.f)){
            upd.m_stepOnce_=true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        if(!upd.m_backwardsStepping)
            ImGui::TextWrapped("Enable Backwards Stepping (above) to step back.");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Pause physics to step frame-by-frame. Hotkeys are in Settings > Keybinds.");
        ImGui::PopStyleColor();
    }
}

void MenuInterface::drawRenderTab(){
    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    struct ResPreset{const char* name;int w,h;};
    static const ResPreset presets[]=
        {{"720p (1280x720)",1280,720},{"1080p (1920x1080)",1920,1080},
         {"1440p (2560x1440)",2560,1440},{"4K (3840x2160)",3840,2160}};
    if(!renderBufsInit)loadRenderSettings();
    float iW=ImGui::GetContentRegionAvail().x*0.45f;

        Widgets::SectionHeader("Render Presets",theme);
        static char presetNameBuf[64]="My Preset";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-170);
    ImGui::InputText("##presetName",presetNameBuf,sizeof(presetNameBuf));
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Save##preset",ImVec2(76,0),theme,anim)){
                std::string pk=std::string("rp_")+presetNameBuf;
        mod->setSavedValue(pk+"_w",std::string(renderWidthBuf));
        mod->setSavedValue(pk+"_h",std::string(renderHeightBuf));
        mod->setSavedValue(pk+"_fps",std::string(renderFpsBuf));
        mod->setSavedValue(pk+"_codec",std::string(renderCodecBuf));
        mod->setSavedValue(pk+"_bitrate",std::string(renderBitrateBuf));
        mod->setSavedValue(pk+"_ext",std::string(renderExtBuf));
        mod->setSavedValue(pk+"_args",std::string(renderArgsBuf));
        mod->setSavedValue(pk+"_pixfmt",std::string(renderPixFmtBuf));
        mod->setSavedValue(pk+"_vargs",std::string(renderVideoArgsBuf));
        mod->setSavedValue(pk+"_aargs",std::string(renderAudioArgsBuf));
        mod->setSavedValue(pk+"_safter",std::string(renderSecondsAfterBuf));
        mod->setSavedValue(pk+"_acodec",std::string(renderAudioCodecBuf));
        mod->setSavedValue(pk+"_abitrate",std::string(renderAudioBitrateBuf));
                auto existing=mod->getSavedValue<std::string>("rp_list","");
        if(existing.find(std::string(presetNameBuf)+"|")==std::string::npos)
            mod->setSavedValue("rp_list",existing+presetNameBuf+"|");
    }
    ImGui::SameLine(0,6);
    if(Widgets::StyledButton("Load##preset",ImVec2(76,0),theme,anim)){
        std::string pk=std::string("rp_")+presetNameBuf;
        if(mod->hasSavedValue(pk+"_w")){
            snprintf(renderWidthBuf,sizeof(renderWidthBuf),"%s",mod->getSavedValue<std::string>(pk+"_w","1920").c_str());
            snprintf(renderHeightBuf,sizeof(renderHeightBuf),"%s",mod->getSavedValue<std::string>(pk+"_h","1080").c_str());
            snprintf(renderFpsBuf,sizeof(renderFpsBuf),"%s",mod->getSavedValue<std::string>(pk+"_fps","60").c_str());
            snprintf(renderCodecBuf,sizeof(renderCodecBuf),"%s",mod->getSavedValue<std::string>(pk+"_codec","").c_str());
            snprintf(renderBitrateBuf,sizeof(renderBitrateBuf),"%s",mod->getSavedValue<std::string>(pk+"_bitrate","30").c_str());
            snprintf(renderExtBuf,sizeof(renderExtBuf),"%s",mod->getSavedValue<std::string>(pk+"_ext",".mp4").c_str());
            snprintf(renderArgsBuf,sizeof(renderArgsBuf),"%s",mod->getSavedValue<std::string>(pk+"_args","-pix_fmt yuv420p").c_str());
            snprintf(renderPixFmtBuf,sizeof(renderPixFmtBuf),"%s",mod->getSavedValue<std::string>(pk+"_pixfmt","yuv420p").c_str());
            snprintf(renderVideoArgsBuf,sizeof(renderVideoArgsBuf),"%s",mod->getSavedValue<std::string>(pk+"_vargs","").c_str());
            snprintf(renderAudioArgsBuf,sizeof(renderAudioArgsBuf),"%s",mod->getSavedValue<std::string>(pk+"_aargs","").c_str());
            snprintf(renderSecondsAfterBuf,sizeof(renderSecondsAfterBuf),"%s",mod->getSavedValue<std::string>(pk+"_safter","3").c_str());
            snprintf(renderAudioCodecBuf,sizeof(renderAudioCodecBuf),"%s",mod->getSavedValue<std::string>(pk+"_acodec","aac").c_str());
            snprintf(renderAudioBitrateBuf,sizeof(renderAudioBitrateBuf),"%s",mod->getSavedValue<std::string>(pk+"_abitrate","192k").c_str());
        }
    }
        {
        auto list=mod->getSavedValue<std::string>("rp_list","");
        if(!list.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::Text("Saved: %s",list.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Render",theme);
    ImGui::Text("Output Name");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##rName",renderNameBuf,sizeof(renderNameBuf)))
        mod->setSavedValue("render_name",std::string(renderNameBuf));

        ImGui::Text("Output Folder");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##rFolder",outputFolderBuf,sizeof(outputFolderBuf),
        ImGuiInputTextFlags_AutoSelectAll))
        mod->setSavedValue("render_output_folder",std::string(outputFolderBuf));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Type or paste a path. Leave blank to use GD/renders/");
    ImGui::PopStyleColor();
    float obtnW=(ImGui::GetContentRegionAvail().x-8)*0.5f;
    if(Widgets::StyledButton("Create & Open##folder",ImVec2(obtnW,0),theme,anim)){
        std::filesystem::path folderPath(outputFolderBuf);
        if(folderPath.empty())folderPath=dirs::getGameDir()/"renders";
        std::error_code ec;
        if(!std::filesystem::exists(folderPath,ec))
            std::filesystem::create_directories(folderPath,ec);
        utils::file::openFolder(folderPath);}
    ImGui::SameLine(0,8);
    if(Widgets::StyledButton("Clear##folder",ImVec2(obtnW,0),theme,anim)){
        outputFolderBuf[0]=0;
        mod->setSavedValue("render_output_folder",std::string(""));}
    ImGui::Dummy(ImVec2(0,4));
            auto* sl = SLRenderer::get();
    if(sl->isRecording()){
        Widgets::StatusBadge("Rendering",ImVec4(0.9f,0.3f,0.3f,1.f));
        ImGui::Dummy(ImVec2(0,4));
        if(Widgets::StyledButton("Stop Render",ImVec2(-1,36),theme,anim))sl->signalStop();
    }else{
        if(Widgets::StyledButton("Start Render",ImVec2(-1,36),theme,anim)){
            sl->loadSettingsFromGeode();
            sl->queueStart();
        }}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Resolution",theme);
    ImGui::Text("Preset");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##rPreset",presets[renderPresetIndex].name)){
        for(int i=0;i<4;i++){bool sel=(renderPresetIndex==i);
            if(ImGui::Selectable(presets[i].name,sel)){renderPresetIndex=i;
                snprintf(renderWidthBuf,sizeof(renderWidthBuf),"%d",presets[i].w);
                snprintf(renderHeightBuf,sizeof(renderHeightBuf),"%d",presets[i].h);}
            if(sel)ImGui::SetItemDefaultFocus();}
        ImGui::EndCombo();}
    ImGui::Text("FPS");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##rFPS",renderFpsBuf,sizeof(renderFpsBuf),ImGuiInputTextFlags_CharsDecimal);
        {
        struct QPreset{const char* n;int w,h,f;const char* br;};
        static const QPreset qp[]={{"1080p60",1920,1080,60,"16"},{"1440p60",2560,1440,60,"24"},{"4K60",3840,2160,60,"50"}};
        float qw=(ImGui::GetContentRegionAvail().x-16)/3.f;
        for(int i=0;i<3;i++){
            if(i)ImGui::SameLine(0,8);
            if(Widgets::StyledButton(qp[i].n,ImVec2(qw,24),theme,anim)){
                snprintf(renderWidthBuf,sizeof(renderWidthBuf),"%d",qp[i].w);
                snprintf(renderHeightBuf,sizeof(renderHeightBuf),"%d",qp[i].h);
                snprintf(renderFpsBuf,sizeof(renderFpsBuf),"%d",qp[i].f);
                snprintf(renderBitrateBuf,sizeof(renderBitrateBuf),"%s",qp[i].br);
                mod->setSavedValue("render_width",(int64_t)qp[i].w);
                mod->setSavedValue("render_height",(int64_t)qp[i].h);
                mod->setSavedValue("render_fps",(int64_t)qp[i].f);
                mod->setSavedValue("render_bitrate",std::string(qp[i].br));
            }
        }
    }
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Encoding",theme);

        static const char* vCodecs[]={"libx264","libx265","h264_nvenc","hevc_nvenc",
        "h264_amf","hevc_amf","h264_qsv","hevc_qsv","libvpx-vp9","av1_nvenc","libaom-av1"};
    static const int vCodecCount=11;
    static char vCodecSearch[64]="";
    ImGui::Text("Video Codec");ImGui::SameLine(iW);
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##vCodecCombo",renderCodecBuf[0]?renderCodecBuf:"Select...")){
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##vCodecSearch",vCodecSearch,sizeof(vCodecSearch));
        ImGui::Separator();
        std::string vSearch(vCodecSearch);
        std::transform(vSearch.begin(),vSearch.end(),vSearch.begin(),::tolower);
        for(int i=0;i<vCodecCount;i++){
            std::string cn(vCodecs[i]);
            std::string cnl=cn;std::transform(cnl.begin(),cnl.end(),cnl.begin(),::tolower);
            if(!vSearch.empty()&&cnl.find(vSearch)==std::string::npos)continue;
            bool sel=(cn==renderCodecBuf);
            if(ImGui::Selectable(vCodecs[i],sel)){
                snprintf(renderCodecBuf,sizeof(renderCodecBuf),"%s",vCodecs[i]);
                mod->setSavedValue("render_codec",std::string(renderCodecBuf));}
            if(sel)ImGui::SetItemDefaultFocus();}
        ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Or type custom:");ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if(ImGui::InputText("##vCodecCustom",renderCodecBuf,sizeof(renderCodecBuf),
            ImGuiInputTextFlags_EnterReturnsTrue))
            mod->setSavedValue("render_codec",std::string(renderCodecBuf));
        ImGui::EndCombo();}

    ImGui::Text("Bitrate (M)");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##rBitrate",renderBitrateBuf,sizeof(renderBitrateBuf));
    ImGui::Text("Extension");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##rExt",renderExtBuf,sizeof(renderExtBuf));
    ImGui::Text("Pixel Format");ImGui::SameLine(iW);ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##rPixFmt",renderPixFmtBuf,sizeof(renderPixFmtBuf)))
        mod->setSavedValue("render_pix_fmt",std::string(renderPixFmtBuf));

        static const char* aCodecs[]={"aac","mp3","opus","flac","ac3","eac3","vorbis","pcm_s16le","copy"};
    static const int aCodecCount=9;
    static const char* aCodecDescs[]={"AAC (recommended)","MP3","Opus (great quality)","FLAC (lossless)",
        "AC3 (Dolby)","E-AC3","Vorbis (OGG)","PCM WAV (uncompressed)","Copy stream as-is"};
    static char aCodecSearch[64]="";
    ImGui::Text("Audio Codec");ImGui::SameLine(iW);
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##aCodecCombo",renderAudioCodecBuf[0]?renderAudioCodecBuf:"Select...")){
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##aCodecSearch",aCodecSearch,sizeof(aCodecSearch));
        ImGui::Separator();
        std::string aSearch(aCodecSearch);
        std::transform(aSearch.begin(),aSearch.end(),aSearch.begin(),::tolower);
        for(int i=0;i<aCodecCount;i++){
            std::string cn(aCodecs[i]);
            std::string cnl=cn;std::transform(cnl.begin(),cnl.end(),cnl.begin(),::tolower);
            if(!aSearch.empty()&&cnl.find(aSearch)==std::string::npos)continue;
            bool sel=(cn==renderAudioCodecBuf);
                        char label[128];snprintf(label,sizeof(label),"%s  —  %s",aCodecs[i],aCodecDescs[i]);
            if(ImGui::Selectable(label,sel)){
                snprintf(renderAudioCodecBuf,sizeof(renderAudioCodecBuf),"%s",aCodecs[i]);
                mod->setSavedValue("render_audio_codec",std::string(renderAudioCodecBuf));}
            if(sel)ImGui::SetItemDefaultFocus();}
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Or type custom:");ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if(ImGui::InputText("##aCodecCustom",renderAudioCodecBuf,sizeof(renderAudioCodecBuf),
            ImGuiInputTextFlags_EnterReturnsTrue))
            mod->setSavedValue("render_audio_codec",std::string(renderAudioCodecBuf));
        ImGui::EndCombo();}

        static const char* aBitrates[]={"96k","128k","192k","256k","320k","512k"};
    static const int aBitrateCount=6;
    ImGui::Text("Audio Bitrate");ImGui::SameLine(iW);
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##aBitrateCombo",renderAudioBitrateBuf[0]?renderAudioBitrateBuf:"192k")){
        for(int i=0;i<aBitrateCount;i++){
            bool sel=(std::string(aBitrates[i])==renderAudioBitrateBuf);
            if(ImGui::Selectable(aBitrates[i],sel)){
                snprintf(renderAudioBitrateBuf,sizeof(renderAudioBitrateBuf),"%s",aBitrates[i]);
                mod->setSavedValue("render_audio_bitrate",std::string(renderAudioBitrateBuf));}
            if(sel)ImGui::SetItemDefaultFocus();}
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Or type custom:");ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if(ImGui::InputText("##aBitrateCustom",renderAudioBitrateBuf,sizeof(renderAudioBitrateBuf),
            ImGuiInputTextFlags_EnterReturnsTrue))
            mod->setSavedValue("render_audio_bitrate",std::string(renderAudioBitrateBuf));
        ImGui::EndCombo();}
    ImGui::Dummy(ImVec2(0,4));
    ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Video Args (passed to -vf)");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##rVArgs",renderVideoArgsBuf,sizeof(renderVideoArgsBuf)))
        mod->setSavedValue("render_video_args",std::string(renderVideoArgsBuf));
    ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Audio Args (extra ffmpeg audio flags)");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##rAArgs",renderAudioArgsBuf,sizeof(renderAudioArgsBuf)))
        mod->setSavedValue("render_audio_args",std::string(renderAudioArgsBuf));
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Audio",theme);
    if(Widgets::ToggleSwitch("Include Audio",&renderIncludeAudio,theme,anim))mod->setSavedValue("render_include_audio",renderIncludeAudio);
    if(Widgets::ToggleSwitch("Auto Color Fix",&renderColorFix,theme,anim))mod->setSavedValue("render_color_fix",renderColorFix);
    if(Widgets::ToggleSwitch("Include Click Sounds",&renderIncludeClicks,theme,anim))mod->setSavedValue("render_include_clicks",renderIncludeClicks);
    if(renderIncludeClicks){
        Widgets::StyledSliderFloat("Music Volume",&renderMusicVol,0.f,2.f,theme,true);
        Widgets::StyledSliderFloat("SFX Volume",&renderSfxVol,0.f,2.f,theme,true);}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Options",theme);
    if(Widgets::ToggleSwitch("Hide End Screen",&renderHideEndscreen,theme,anim))mod->setSavedValue("render_hide_endscreen",renderHideEndscreen);
    if(Widgets::ToggleSwitch("Hide Level Complete",&renderHideLevelComplete,theme,anim))mod->setSavedValue("render_hide_levelcomplete",renderHideLevelComplete);
    ImGui::Dummy(ImVec2(0,4));
    {
        std::error_code ffec;
        auto ffSave=Mod::get()->getSaveDir()/"ffmpeg.exe";
        auto ffRes=Mod::get()->getResourcesDir()/"ffmpeg.exe";
        bool ffFound=std::filesystem::exists(ffSave,ffec)||std::filesystem::exists(ffRes,ffec);
        if(ffFound){
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.3f,1.f,0.3f,1.f));
            ImGui::TextWrapped("ffmpeg.exe found.");
            ImGui::PopStyleColor();
        }else{
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,0.8f,0.2f,0.8f));
            ImGui::TextWrapped("ffmpeg.exe not found. Place ffmpeg.exe in the mod resources or save folder.");
            ImGui::PopStyleColor();
        }
    }

        ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Playback Fixes",theme);
    Widgets::ToggleSwitch("Scroll Speed Fix",&engine->updater.m_ssbFix,theme,anim);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Prevents scroll speed desync at high TPS. Recommended when rendering.");
    ImGui::PopStyleColor();
}

void MenuInterface::drawClicksTab(){
    auto* csm=ClickSoundManager::get();
    auto* mod=Mod::get();
    if(!clickPacksScanned){
        csm->scanClickPacks();csm->scanClickPacksP2();clickPacksScanned=true;
        if(!csm->activePackName.empty())
            for(int i=0;i<(int)csm->availablePacks.size();i++)
                if(csm->availablePacks[i]==csm->activePackName){clickPackIndex=i;break;}
        if(!csm->activePackNameP2.empty())
            for(int i=0;i<(int)csm->availablePacksP2.size();i++)
                if(csm->availablePacksP2[i]==csm->activePackNameP2){clickPackIndexP2=i;break;}}
    ImGui::Dummy(ImVec2(0,4));
    if(Widgets::ModuleCard("Click Sounds","Play click and release sounds on input",&csm->enabled,theme,anim))
        mod->setSavedValue("click_enabled",csm->enabled);
    ImGui::Dummy(ImVec2(0,6));
    Widgets::SectionHeader("Click Pack",theme);
    float bw=80.f,cw=ImGui::GetContentRegionAvail().x-bw-8.f;
    if(csm->availablePacks.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.7f,0.7f,0.7f,0.6f));
        ImGui::TextWrapped("No click packs found. Use Open Folder to add packs.");
        ImGui::PopStyleColor();
    }else{
        ImGui::SetNextItemWidth(cw);
        if(ImGui::BeginCombo("##cp",csm->availablePacks[clickPackIndex].c_str())){
            for(int i=0;i<(int)csm->availablePacks.size();i++){
                bool sel=(clickPackIndex==i);
                if(ImGui::Selectable(csm->availablePacks[i].c_str(),sel)){
                    clickPackIndex=i;csm->activePackName=csm->availablePacks[i];
                    csm->loadClickPack(csm->activePackName,csm->p1Pack);
                    mod->setSavedValue("click_pack",csm->activePackName);}
                if(sel)ImGui::SetItemDefaultFocus();}
            ImGui::EndCombo();}
        ImGui::SameLine();}
    if(Widgets::StyledButton("Refresh",ImVec2(bw,0),theme,anim)){
        csm->scanClickPacks();clickPackIndex=0;
        if(!csm->availablePacks.empty()){csm->activePackName=csm->availablePacks[0];csm->loadClickPack(csm->activePackName,csm->p1Pack);}}
    ImGui::Dummy(ImVec2(0,4));
    if(Widgets::StyledButton("Open Folder",ImVec2(-1,32),theme,anim))csm->openClickFolder();
    if(!csm->p1Pack.empty()){
        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Hard: %d  Soft: %d  Release: %d  Noise: %d",
            csm->p1Pack.hardCount(),csm->p1Pack.softCount(),csm->p1Pack.releaseCount(),csm->p1Pack.noiseCount());
        ImGui::PopStyleColor();}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Volume",theme);
    if(!csm->p1Pack.hardClicks.empty())
        if(Widgets::StyledSliderFloat("Hard Click",&csm->p1Pack.hardVolume,0.f,2.f,theme,true))
            mod->setSavedValue("click_hard_vol",(double)csm->p1Pack.hardVolume);
    if(!csm->p1Pack.softClicks.empty())
        if(Widgets::StyledSliderFloat("Soft Click",&csm->p1Pack.softVolume,0.f,2.f,theme,true))
            mod->setSavedValue("click_soft_vol",(double)csm->p1Pack.softVolume);
    if(csm->p1Pack.releaseCount()>0)
        if(Widgets::StyledSliderFloat("Release",&csm->p1Pack.releaseVolume,0.f,2.f,theme,true))
            mod->setSavedValue("click_release_vol",(double)csm->p1Pack.releaseVolume);
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Behavior",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Softness -- 0 = always hard,  1 = always soft clicks");
    ImGui::PopStyleColor();
    if(Widgets::StyledSliderFloat("##softness",&csm->softness,0.f,1.f,theme))mod->setSavedValue("click_softness",(double)csm->softness);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Click Delay -- adds a random timing offset to sound more human");
    ImGui::PopStyleColor();
    if(Widgets::StyledSliderFloat("##delaymin",&csm->clickDelayMin,0.f,100.f,theme)){
        if(csm->clickDelayMin>csm->clickDelayMax)csm->clickDelayMax=csm->clickDelayMin;
        mod->setSavedValue("click_delay_min",(double)csm->clickDelayMin);}
    if(Widgets::StyledSliderFloat("##delaymax",&csm->clickDelayMax,0.f,100.f,theme)){
        if(csm->clickDelayMax<csm->clickDelayMin)csm->clickDelayMin=csm->clickDelayMax;
        mod->setSavedValue("click_delay_max",(double)csm->clickDelayMax);}
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Min: %.0f ms    Max: %.0f ms",csm->clickDelayMin,csm->clickDelayMax);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,4));
    if(Widgets::ToggleSwitch("Play During Playback",&csm->playDuringPlayback,theme,anim))mod->setSavedValue("click_play_during_playback",csm->playDuringPlayback);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("When off, click sounds are silent during macro playback.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Background Noise",theme);
    if(csm->p1Pack.noiseFiles.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.7f,0.7f,0.7f,0.6f));
        ImGui::TextWrapped("No noise files. Add a 'noise' folder to your click pack.");
        ImGui::PopStyleColor();
    }else{
        if(Widgets::ToggleSwitch("Enable Background Noise",&csm->backgroundNoiseEnabled,theme,anim)){
            mod->setSavedValue("click_bg_noise",csm->backgroundNoiseEnabled);
            if(csm->backgroundNoiseEnabled)csm->startBackgroundNoise();else csm->stopBackgroundNoise();}
        if(Widgets::StyledSliderFloat("Noise Volume",&csm->backgroundNoiseVolume,0.f,2.f,theme,true)){
            mod->setSavedValue("click_bg_noise_vol",(double)csm->backgroundNoiseVolume);
            if(csm->bgNoiseChannel)csm->bgNoiseChannel->setVolume(csm->backgroundNoiseVolume);}}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Player 2",theme);
    if(Widgets::ToggleSwitch("Separate P2 Clicks",&csm->separateP2Clicks,theme,anim))mod->setSavedValue("click_separate_p2",csm->separateP2Clicks);
    if(csm->separateP2Clicks){
        ImGui::Dummy(ImVec2(0,6));Widgets::SectionHeader("P2 Click Pack",theme);
        float p2bw=80.f,p2cw=ImGui::GetContentRegionAvail().x-p2bw-8.f;
        if(!csm->availablePacksP2.empty()){
            ImGui::SetNextItemWidth(p2cw);
            if(ImGui::BeginCombo("##cpp2",csm->availablePacksP2[clickPackIndexP2].c_str())){
                for(int i=0;i<(int)csm->availablePacksP2.size();i++){bool sel=(clickPackIndexP2==i);
                    if(ImGui::Selectable(csm->availablePacksP2[i].c_str(),sel)){
                        clickPackIndexP2=i;csm->activePackNameP2=csm->availablePacksP2[i];
                        csm->loadClickPack(csm->activePackNameP2,csm->p2Pack,true);
                        mod->setSavedValue("click_pack_p2",csm->activePackNameP2);}
                    if(sel)ImGui::SetItemDefaultFocus();}
                ImGui::EndCombo();}ImGui::SameLine();}
        if(Widgets::StyledButton("Refresh##p2",ImVec2(p2bw,0),theme,anim)){
            csm->scanClickPacksP2();clickPackIndexP2=0;
            if(!csm->availablePacksP2.empty()){csm->activePackNameP2=csm->availablePacksP2[0];csm->loadClickPack(csm->activePackNameP2,csm->p2Pack,true);}}
        ImGui::Dummy(ImVec2(0,4));
        if(Widgets::StyledButton("Open P2 Folder",ImVec2(-1,32),theme,anim))csm->openClickFolderP2();}}

void MenuInterface::drawAutoclickerTab(){
    auto* ac=Autoclicker::get();
    auto* mod=Mod::get();
    auto* eng=GucciEngine::get();
    ImGui::Dummy(ImVec2(0,4));
    if(Widgets::ModuleCard("Autoclicker","Auto-click at configurable intervals",&ac->enabled,theme,anim))
        mod->setSavedValue("ac_enabled",ac->enabled);
    if(ac->enabled && eng->isPlaying()){Widgets::StatusBadge("PAUSED (PLAYBACK)",ImVec4(0.8f,0.6f,0.2f,1.f));}
    else if(ac->enabled){Widgets::StatusBadge("ACTIVE",ImVec4(0.3f,1.f,0.4f,1.f));}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Players",theme);
    if(Widgets::ToggleSwitch("Player 1",&ac->player1,theme,anim))mod->setSavedValue("ac_player1",ac->player1);
    if(Widgets::ToggleSwitch("Player 2",&ac->player2,theme,anim))mod->setSavedValue("ac_player2",ac->player2);
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Timing",theme);
    if(Widgets::StyledSliderInt("Hold Ticks",&ac->holdTicks,1,120,theme))mod->setSavedValue("ac_hold_ticks",ac->holdTicks);
    if(Widgets::StyledSliderInt("Release Ticks",&ac->releaseTicks,1,120,theme))mod->setSavedValue("ac_release_ticks",ac->releaseTicks);
    float cps=(float)eng->updater.m_tps/(float)(ac->holdTicks+ac->releaseTicks);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("~%.1f clicks/sec at %.0f TPS",cps,eng->updater.m_tps);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Options",theme);
    if(Widgets::ToggleSwitch("Only While Holding",&ac->onlyWhileHolding,theme,anim))mod->setSavedValue("ac_only_holding",ac->onlyWhileHolding);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("When enabled, only auto-clicks while you hold the jump button.");
    ImGui::PopStyleColor();}

void MenuInterface::drawSettingsTab(){
    auto* eng=GucciEngine::get();
    Widgets::SectionHeader("Interface",theme);
    if(Widgets::ToggleSwitch("MegaHack-Style Menu",&megaHackLook,theme,anim))
        Mod::get()->setSavedValue("ui_megahack_look",megaHackLook);
    if(Widgets::ToggleSwitch("Compact Mode",&compactMode,theme,anim))
        Mod::get()->setSavedValue("ui_compact_mode",compactMode);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("A small corner panel instead of the full menu -- record/play, TPS/speed, frame step, noclip, and a few other essentials, small enough to leave open while actually playing.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));

        Widgets::SectionHeader("Diagnostics",theme);
    {
                ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
        ImGui::Text("Build: %s", GB_BUILD_LABEL);
        ImGui::PopStyleColor();
        bool healthy = gbcheck::g_ranOnce && gbcheck::g_failCount==0;
        ImVec4 statusCol = !gbcheck::g_ranOnce ? theme.textSecondary
            : (healthy ? ImVec4(0.3f,0.85f,0.3f,1.f) : ImVec4(0.95f,0.3f,0.3f,1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,statusCol);
        if(!gbcheck::g_ranOnce) ImGui::TextUnformatted("Self-check: not yet run");
        else if(healthy) ImGui::Text("Self-check: PASSED (%d checks)",gbcheck::g_passCount);
        else ImGui::Text("Self-check: FAILED (%d of %d failed)",
            gbcheck::g_failCount,gbcheck::g_passCount+gbcheck::g_failCount);
        ImGui::PopStyleColor();
                for(auto const& r:gbcheck::g_results){
            ImGui::PushStyleColor(ImGuiCol_Text,r.passed?ImVec4(0.3f,0.8f,0.3f,1.f):ImVec4(0.95f,0.35f,0.35f,1.f));
            ImGui::Text("  %s  %s",r.passed?"[OK]":"[X]",r.name.c_str());
            ImGui::PopStyleColor();
        }
                ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        const char* mode=eng->isRecording()?"Recording":eng->isPlaying()?"Playing":"Idle";
        ImGui::Text("Mode: %s   Stored frames: %zu",mode,eng->practiceFix.m_storedFrames.size());
        ImGui::Text("Frame-window marks: %zu   Analyzed: %s",
            eng->fwMarks.size(),eng->fwHasData?"yes":"no");
        ImGui::Text("Renderer: %s",eng->renderer.recording?"RECORDING":"idle");
        if(eng->renderer.lastRender.fileSize>0)
            ImGui::Text("Last render: %.2f MB",(double)eng->renderer.lastRender.fileSize/(1024.0*1024.0));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0,4));
        if(Widgets::ToggleSwitch("Log Frame Increments",&eng->updater.m_logFrameIncrements,theme,anim))
            Mod::get()->setSavedValue("diag_log_frame_increments",eng->updater.m_logFrameIncrements);
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Juice's frame-skip report: logs every place the frame counter advances, tagged by call site, to the mod log. Flip on right before reproducing (an MH step, a release test), then off -- leaving it on floods the log during normal play.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Theme",theme);
        int pc=ThemeEngine::getPresetCount();
    const ThemePreset* presets=ThemeEngine::getPresets();
    float colW=(ImGui::GetContentRegionAvail().x-8.f)*0.5f;
    float btnH=32.f;
    for(int i=0;i<pc;i++){
        if(i%2==1)ImGui::SameLine(0,8);
        bool active=(theme.activePreset==i);
        if(Widgets::PillButton(presets[i].name,active,colW,theme,anim)){
            theme.applyPreset(i);
            if(i==0)activeTheme=THEME_GUCCI;
            else if(i==1)activeTheme=THEME_TOOSII;
            else if(i==2)activeTheme=THEME_TOOSII_SYRACUSE;
            else if(i==3)activeTheme=THEME_TOOSII_SACSTATE;
            else if(i==4)activeTheme=THEME_JA;
            else if(i==5)activeTheme=THEME_GIDDEY;
            else if(i==6)activeTheme=THEME_BAM;
            else if(i==7)activeTheme=THEME_SEXYY;
            else if(i==8)activeTheme=THEME_JUICE;
            else if(i==9)activeTheme=THEME_BUTLER;
            saveSettings();}
        if(i%2==0&&i+1>=pc)ImGui::Dummy(ImVec2(0,0));
    }
    ImGui::Text("Accent Color");ImGui::SameLine();
    if(ImGui::ColorEdit4("##acc",(float*)&theme.accentColor,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel))theme.activePreset=-1;
    ImGui::Dummy(ImVec2(0,4));
    ImGui::Text("Background Color");ImGui::SameLine();
    if(ImGui::ColorEdit4("##bgc",(float*)&theme.bgColor,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel))theme.activePreset=-1;
    ImGui::Dummy(ImVec2(0,4));
    ImGui::Text("Card Color");ImGui::SameLine();
    if(ImGui::ColorEdit4("##cdc",(float*)&theme.cardColor,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel))theme.activePreset=-1;
    ImGui::Dummy(ImVec2(0,8));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Corner Rounding  (0 = sharp,  16 = fully rounded)");
    ImGui::PopStyleColor();
    Widgets::StyledSliderFloat("##cornerRadius",&theme.cornerRadius,0.f,16.f,theme);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Background Opacity  (0.5 = translucent,  1.0 = solid)");
    ImGui::PopStyleColor();
    Widgets::StyledSliderFloat("##bgOpacity",&theme.bgOpacity,0.5f,1.f,theme);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("Animation Speed  (2 = slow and smooth,  24 = snappy)");
    ImGui::PopStyleColor();
    Widgets::StyledSliderFloat("##animSpeed",&anim.animSpeed,2.f,24.f,theme);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::Text("Open Animation");
    const char* animNames[]={"Center","From Left","From Right","From Top","From Bottom"};
    int dir=(int)anim.openDirection;ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##animDir",&dir,animNames,5))anim.openDirection=(AnimDirection)dir;
    ImGui::Dummy(ImVec2(0,4));
    Widgets::ToggleSwitch("Glow Color Cycle",&theme.glowCycleEnabled,theme,anim);
    if(theme.glowCycleEnabled){ImGui::Dummy(ImVec2(0,4));Widgets::StyledSliderFloat("Cycle Speed",&theme.glowCycleRate,0.02f,1.f,theme);}
    ImGui::Dummy(ImVec2(0,4));
    Widgets::ToggleSwitch("Ambient Waves",&ambientWavesEnabled,theme,anim);
    ImGui::Dummy(ImVec2(0,12));
    Widgets::SectionHeader("Bot Settings Presets",theme);
    {
        static char presetNameBuf[64] = {};
        ImGui::SetNextItemWidth(-80.f);
        ImGui::InputText("##presetName",presetNameBuf,sizeof(presetNameBuf));
        ImGui::SameLine(0,6);
        if(Widgets::StyledButton("Save",ImVec2(70,26),theme,anim)&&presetNameBuf[0]){
            eng->saveBotSettingsPreset(presetNameBuf);
        }
        ImGui::Dummy(ImVec2(0,4));
        for(auto& preset : eng->settingsPresets){
            float pw=(ImGui::GetContentRegionAvail().x-10)/2.f;
            if(Widgets::StyledButton(preset.name.c_str(),ImVec2(pw,26),theme,anim))
                eng->loadBotSettingsPreset(preset.name);
            ImGui::SameLine(0,10);
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.7f,0.1f,0.1f,0.8f));
            if(ImGui::Button(("X##del_"+preset.name).c_str(),ImVec2(pw,26)))
                eng->deleteBotSettingsPreset(preset.name);
            ImGui::PopStyleColor();
        }
        if(eng->settingsPresets.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("No presets saved yet. Type a name and press Save.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::Dummy(ImVec2(0,12));
    Widgets::SectionHeader("Advanced",theme);
    Widgets::ModuleCard("FastPlayback","Start playback without restarting the level",&eng->fastPlayback,theme,anim);
    ImGui::Dummy(ImVec2(0,12));

    Widgets::SectionHeader("Fun",theme);
    {
        auto* brrr=BigBrrrManager::get();
        bool brrrOn=brrr->enabled;
        if(Widgets::ToggleSwitch("BIG BRRRR",&brrrOn,theme,anim))brrr->setEnabled(brrrOn);
        ImGui::SameLine();
        if(Widgets::StyledButton("Open BRRRR Folder",ImVec2(160,0),theme,anim))brrr->openBrrrFolder();
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        if(brrr->hasFile())
            ImGui::TextWrapped("Loops the audio file in your BRRRR folder (overriding the bundled one) and makes the whole menu bounce. Doesn't touch the Jupiter tab.");
        else
            ImGui::TextWrapped("Loops the bundled BRRRR track and makes the whole menu bounce. Doesn't touch the Jupiter tab. Drop your own mp3/wav/ogg in the BRRRR folder to swap it without a rebuild.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,12));

    Widgets::SectionHeader("Keybinds",theme);
    struct{const char* label;int* ptr;}kbs[]={
        {"Menu Toggle",&keybinds.menu},{"Frame Advance",&keybinds.frameAdvance},
        {"Frame Step",&keybinds.frameStep},{"Replay Toggle",&keybinds.replayToggle},
        {"Noclip",&keybinds.noclip},{"Safe Mode",&keybinds.safeMode},
        {"Trajectory",&keybinds.trajectory},{"Hitboxes",&keybinds.hitboxes},
        {"Audio Pitch",&keybinds.audioPitch},{"RNG Lock",&keybinds.rngLock},
        {"Layout Mode",&keybinds.layoutMode},{"No Mirror",&keybinds.noMirror},
        {"Autoclicker",&keybinds.autoclicker},
        {"Intentional Death",&keybinds.intentionalDeath},
        {"Back Step",&keybinds.backStep},
        {"Auto-Flip",&keybinds.autoFlip},
        {"Prevent Death",&keybinds.preventDeath},
        {"Mirror Inputs",&keybinds.mirrorInputs},
        {"Compact Mode",&keybinds.compactMode}};
    for(auto& e:kbs){ImGui::Dummy(ImVec2(0,4));Widgets::KeybindButton(e.label,e.ptr,theme,anim);}
    ImGui::Dummy(ImVec2(0,12));
    if(Widgets::StyledButton("Reset to Defaults",ImVec2(-1,32),theme,anim)){
        theme.resetDefaults();activeTheme=THEME_GUCCI;
        anim.animSpeed=8.f;anim.openDirection=ANIM_CENTER;
        eng->fastPlayback=false;
        ambientWavesEnabled=true;saveSettings();}}

void MenuInterface::drawMoreHacksTab(){
    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    Widgets::GucciQuote("\"They asked how many attempts. I said don't worry about it.\"",
        "-- GucciBot, attempt counter hidden",theme);
    ImGui::Dummy(ImVec2(0,4));

    Widgets::SectionHeader("Display",theme);
    if(Widgets::ToggleSwitch("Hide Attempt Counter",&engine->hackHideAttempts,theme,anim))
        mod->setSavedValue("hack_hide_attempts",engine->hackHideAttempts);
    if(Widgets::ToggleSwitch("Hide Percentage (experimental)",&engine->hackHidePercentage,theme,anim))
        mod->setSavedValue("hack_hide_percentage",engine->hackHidePercentage);
    if(Widgets::ToggleSwitch("No Death Flash",&engine->hackNoSpikeFlash,theme,anim))
        mod->setSavedValue("hack_no_flash",engine->hackNoSpikeFlash);

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Respawn",theme);
    if(Widgets::ToggleSwitch("Auto Retry",&engine->hackAutoRetry,theme,anim))
        mod->setSavedValue("hack_auto_retry",engine->hackAutoRetry);
    if(engine->hackAutoRetry){
        if(Widgets::StyledSliderFloat("Retry Delay (s)",&engine->hackAutoRetryDelay,0.f,2.f,theme))
            mod->setSavedValue("hack_auto_retry_delay",(double)engine->hackAutoRetryDelay);
    }
    if(Widgets::ToggleSwitch("Instant Respawn",&engine->hackRespawnInstant,theme,anim))
        mod->setSavedValue("hack_respawn_instant",engine->hackRespawnInstant);

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Controls",theme);
    if(Widgets::ToggleSwitch("Force Platformer Controls (experimental)",&engine->hackForcePlatformer,theme,anim))
        mod->setSavedValue("hack_force_platformer",engine->hackForcePlatformer);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Hide Attempts, No Death Flash, Auto Retry, and Instant Respawn are wired. Hide Percentage and Force Platformer are marked experimental pending verified game bindings.");
    ImGui::PopStyleColor();
}

void MenuInterface::drawIndicatorsTab(){
    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    Widgets::GucciQuote("\"Green means go. Red means don't.\"","-- Survival Indicator",theme);
    ImGui::Dummy(ImVec2(0,4));

    if(Widgets::ToggleSwitch("Enable Survival Indicator",&engine->survivalIndicator,theme,anim))
        mod->setSavedValue("hack_survival_indicator",engine->survivalIndicator);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("A marker on the player that turns green when the next click keeps you alive for the lookahead window, red otherwise. Runs on its own -- doesn't need \"Show Trajectory\" enabled.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));

    if(!engine->survivalIndicator)return;

    Widgets::SectionHeader("Style",theme);
    const char* styles[]={"Ring","Classic","Converge","Pulse"};
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##indicatorStyle",&engine->indicatorStyle,styles,4))
        mod->setSavedValue("hack_indicator_style",engine->indicatorStyle);

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Timing",theme);
    if(Widgets::StyledSliderInt("Lookahead (frames)",&engine->indicatorLookahead,5,120,theme))
        mod->setSavedValue("hack_survival_indicator_lookahead",engine->indicatorLookahead);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("How many frames ahead the indicator checks before calling a click safe.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Appearance",theme);
    if(Widgets::StyledSliderFloat("Opacity",&engine->indicatorOpacity,0.1f,1.f,theme))
        mod->setSavedValue("hack_indicator_opacity",(double)engine->indicatorOpacity);
    ImGui::Text("Safe Colour");ImGui::SameLine();
    {float col[3]={engine->indicatorSafeColorR,engine->indicatorSafeColorG,engine->indicatorSafeColorB};
    if(ImGui::ColorEdit3("##indSafeC",col,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel)){
        engine->indicatorSafeColorR=col[0];engine->indicatorSafeColorG=col[1];engine->indicatorSafeColorB=col[2];
        mod->setSavedValue("hack_indicator_safe_r",(double)col[0]);
        mod->setSavedValue("hack_indicator_safe_g",(double)col[1]);
        mod->setSavedValue("hack_indicator_safe_b",(double)col[2]);}}
    ImGui::SameLine();ImGui::Text("Danger Colour");ImGui::SameLine();
    {float col[3]={engine->indicatorDangerColorR,engine->indicatorDangerColorG,engine->indicatorDangerColorB};
    if(ImGui::ColorEdit3("##indDangerC",col,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel)){
        engine->indicatorDangerColorR=col[0];engine->indicatorDangerColorG=col[1];engine->indicatorDangerColorB=col[2];
        mod->setSavedValue("hack_indicator_danger_r",(double)col[0]);
        mod->setSavedValue("hack_indicator_danger_g",(double)col[1]);
        mod->setSavedValue("hack_indicator_danger_b",(double)col[2]);}}
    if(Widgets::ToggleSwitch("Flash On Click",&engine->indicatorFlashEnabled,theme,anim))
        mod->setSavedValue("hack_indicator_flash",engine->indicatorFlashEnabled);

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Sound",theme);
    if(Widgets::ToggleSwitch("Pitch-Shifted Click Cue",&engine->indicatorSoundEnabled,theme,anim))
        mod->setSavedValue("hack_indicator_sound",engine->indicatorSoundEnabled);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Pitches your existing click sound higher the tighter the window is. Requires Click Sounds enabled (Clicks tab) -- this doesn't add a new sound, it reshapes the one you already have.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Calibration",theme);
    auto& calib=CalibrationService::get();
    static int calibModeSel=0;
    const char* gmNames[GM_Count]={"Cube","Ship","Ball","UFO","Wave","Robot","Spider"};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##calibMode",&calibModeSel,gmNames,GM_Count);
    auto& gcal=calib.modes[calibModeSel];

    if(gcal.sampleCount>0){
        ImGui::Text("Lead: %.0f ms    Jitter: %.0f ms    (%d samples)",gcal.leadMs,gcal.jitterMs,gcal.sampleCount);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextUnformatted("Not calibrated yet.");
        ImGui::PopStyleColor();
    }

    float cbw=(ImGui::GetContentRegionAvail().x-10)/2.f;
    if(calib.active&&calib.calibratingMode==calibModeSel){
        char prog[64];snprintf(prog,sizeof(prog),"Cancel (%d/%d)",calib.repsDone,calib.repsTarget);
        if(Widgets::StyledButton(prog,ImVec2(cbw,28),theme,anim))calib.cancel();
    } else if(!calib.active){
        if(Widgets::StyledButton("Start Calibration",ImVec2(cbw,28),theme,anim)&&PlayLayer::get())
            calib.start(calibModeSel);
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
        Widgets::StyledButton("Start Calibration",ImVec2(cbw,28),theme,anim);
        ImGui::PopStyleVar();
    }
    ImGui::SameLine(0,10);
    if(Widgets::StyledButton("Reset",ImVec2(cbw,28),theme,anim))calib.resetMode(calibModeSel);

    if(calib.active&&calib.calibratingMode==calibModeSel){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("In the level, click steadily along with the cue. %d reps.",calib.repsTarget);
        ImGui::PopStyleColor();
    } else if(!PlayLayer::get()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Enter a level to run calibration -- it needs real clicks to measure against.");
        ImGui::PopStyleColor();
    }

    if(Widgets::ToggleSwitch(("Show Guide In "+std::string(gmNames[calibModeSel])).c_str(),&gcal.guideEnabled,theme,anim))
        calib.save();
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Calibration currently measures and stores your lead/jitter per gamemode. It does not yet shift the indicator's timing -- the indicator's flash/sound fire in the same frame as your real click, so there's nothing to offset against. Told Nigel; revisit if a scheduled/count-in style cue gets added.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Stats",theme);
    if(Widgets::ToggleSwitch("Accuracy / Streak HUD",&engine->accuracyHudEnabled,theme,anim))
        mod->setSavedValue("hack_accuracy_hud",engine->accuracyHudEnabled);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("On-screen readout of how many of your real clicks matched the indicator's safe/unsafe call, plus your current and best streak this level. Only counts clicks while the indicator above is enabled.");
    ImGui::PopStyleColor();
}

// Click-rhythm bar: a fixed white line stays put at the horizontal center
// while the macro's click/hold windows scroll toward and through it at
// constant real-time speed (built from the dedicated jupiterMacro data,
// loaded once at startup -- see loadJupiterMacroData in engine_core.cpp).
// Per Nigel's spec: a block's left edge crossing the line means click, its
// right edge crossing means release.
//
// Completely independent of live gameplay -- no PlayLayer or Playing-mode
// requirement, and no dependency on the general `replay` object either
// (that one drives real bot playback elsewhere and must stay untouched).
// Position is a transport, not just an auto-loop: Pause/Resume/Reset
// buttons plus click-drag-to-skim directly on the bar. jupiterClickBarPosSec
// is the single source of truth, advanced by real elapsed wall-clock time
// each frame while not paused, and jumped directly by dragging.
static void drawJupiterClickBar(ThemeEngine& theme,AnimationState& anim,GucciEngine* engine,float windowSeconds,bool externalWidgetJustReleased,float h=46.f){
    auto& jup=engine->jupiterMacro;
    if(jup.clickIntervalsSec.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("No click data yet.");
        ImGui::PopStyleColor();
        return;
    }

    double maxT=0.0;
    for(auto const& iv:jup.clickIntervalsSec)maxT=std::max(maxT,iv.second);
    double loopLen=std::max(maxT,1.0);

    // Loop OFF: plays through once, then auto-pauses back at the beginning.
    // Loop ON: wraps back to 0 and keeps playing, clearing your own click/
    // release marks each time for a fresh per-lap comparison.
    double realNow=ImGui::GetTime();
    if(!engine->jupiterClickBarPaused){
        double dt=realNow-engine->jupiterClickBarLastRealTime;
        if(dt>0.0&&dt<1.0){
            double newPos=engine->jupiterClickBarPosSec+dt;
            if(newPos>=loopLen){
                engine->jupiterClickBarPosSec=0.0;
                if(engine->jupiterClickBarLoop){
                    engine->jupiterClickBarMyClicks.clear();
                    engine->jupiterClickBarMyReleases.clear();
                } else {
                    engine->jupiterClickBarPaused=true;
                }
            } else {
                engine->jupiterClickBarPosSec=newPos;
            }
        }
    }
    engine->jupiterClickBarLastRealTime=realNow;

    gbju::syncClickBarMusic(true,engine->jupiterClickBarPaused,engine->jupiterClickBarPosSec);

    if(Widgets::StyledButton(engine->jupiterClickBarPaused?"Resume":"Pause",ImVec2(80,24),theme,anim))
        engine->jupiterClickBarPaused=!engine->jupiterClickBarPaused;
    ImGui::SameLine();
    if(Widgets::StyledButton("Reset",ImVec2(70,24),theme,anim)){
        engine->jupiterClickBarPosSec=0.0;
        engine->jupiterClickBarMyClicks.clear();
        engine->jupiterClickBarMyReleases.clear();
    }
    ImGui::SameLine();
    if(Widgets::ToggleSwitch("Loop",&engine->jupiterClickBarLoop,theme,anim))
        Mod::get()->setSavedValue("jupiter_clickbar_loop",engine->jupiterClickBarLoop);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("%.1fs / %.1fs",engine->jupiterClickBarPosSec,loopLen);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,6));

    ImVec2 pos=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x;
    ImDrawList* dl=ImGui::GetWindowDrawList();

    // Your own mouse clicks -- same ImGui mouse path the menu buttons
    // already use, so it's known to work. Keyboard (spacebar/up/W) is
    // tracked separately via keybinds.cpp's existing dispatcher hook --
    // ImGui doesn't reliably see game keys in this GD+ImGui integration,
    // which is exactly why that hook exists instead of relying on ImGui for
    // these.
    //
    // Scoped to the cursor actually being over the bar's own rect, NOT an
    // IsAnyItemHovered()-style "was anything else touched" check -- that
    // was tried first and was wrong two different ways: IsAnyItemHovered()
    // also reads HoveredIdPreviousFrame (see imgui.cpp), so it stayed true
    // for one frame after merely hovering the bar itself (its own skim
    // InvisibleButton covers this same rect), which silently ate real bar
    // clicks; and it can't see a widget in the CALLER (like the Window
    // slider above) that already finished its own release-handling and
    // cleared its active state before this code runs. Position scoping
    // sidesteps both, and covers any future widget on the page for free
    // without needing to enumerate it. externalWidgetJustReleased covers
    // the one case position-scoping alone can't: dragging the Window
    // slider and releasing with the cursor incidentally over the bar.
    bool mouseOverBar=ImGui::IsMouseHoveringRect(pos,ImVec2(pos.x+w,pos.y+h));
    bool blockMark=!mouseOverBar||externalWidgetJustReleased;
    if(!blockMark&&ImGui::IsMouseClicked(ImGuiMouseButton_Left))engine->jupiterClickBarMyClicks.push_back(engine->jupiterClickBarPosSec);
    if(!blockMark&&ImGui::IsMouseReleased(ImGuiMouseButton_Left))engine->jupiterClickBarMyReleases.push_back(engine->jupiterClickBarPosSec);

    const ImU32 barCol=IM_COL32(137,126,94,255); // olive track, per Nigel's reference sketch
    const ImU32 white=IM_COL32(255,255,255,255);
    const ImU32 clickCol=theme.getAccentU32(1.f); // already the JMF gold while this tab is active

    dl->AddRectFilled(pos,ImVec2(pos.x+w,pos.y+h),barCol,4.f);

    float centerX=pos.x+w*0.5f;
    float halfWindow=std::max(windowSeconds,0.2f)*0.5f;
    float pxPerSec=(w*0.5f)/halfWindow;
    double nowSec=engine->jupiterClickBarPosSec;

    // Macro's click/hold windows -- filled yellow boxes.
    for(auto const& iv:jup.clickIntervalsSec){
        double relStart=iv.first-nowSec, relEnd=iv.second-nowSec;
        if(relEnd<-halfWindow||relStart>halfWindow)continue;
        float x0=centerX+(float)relStart*pxPerSec;
        float x1=centerX+(float)relEnd*pxPerSec;
        x0=std::max(x0,pos.x); x1=std::min(x1,pos.x+w);
        if(x1>x0)dl->AddRectFilled(ImVec2(x0,pos.y+5),ImVec2(x1,pos.y+h-5),clickCol,2.f);
    }

    // Your own clicks + releases -- thin white lines, scrolling past the
    // same way the yellow marks do. Press marks rise from the bottom,
    // release marks hang from the top, so the two are visually
    // distinguishable at a glance instead of being identical lines.
    auto drawMyMark=[&](double t,bool isRelease){
        double rel=t-nowSec;
        if(rel<-halfWindow||rel>halfWindow)return;
        float x=centerX+(float)rel*pxPerSec;
        float yMid=pos.y+h*0.5f;
        if(isRelease)dl->AddLine(ImVec2(x,pos.y+3),ImVec2(x,yMid),white,2.f);
        else dl->AddLine(ImVec2(x,yMid),ImVec2(x,pos.y+h-3),white,2.f);
    };
    for(double t:engine->jupiterClickBarMyClicks)drawMyMark(t,false);
    for(double t:engine->jupiterClickBarMyReleases)drawMyMark(t,true);

    dl->AddLine(ImVec2(centerX,pos.y-4),ImVec2(centerX,pos.y+h+4),white,3.f);

    // Skim: click-drag directly on the bar to scrub. Dragging right reveals
    // earlier content (rewind), dragging left reveals later content
    // (fast-forward) -- matches dragging a filmstrip past a fixed gate.
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##clickBarSkim",ImVec2(w,h));
    if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
        engine->jupiterClickBarPaused=true;
        double posSec=engine->jupiterClickBarPosSec-ImGui::GetIO().MouseDelta.x/pxPerSec;
        posSec=std::clamp(posSec,0.0,loopLen);
        engine->jupiterClickBarPosSec=posSec;
    }
    ImGui::Dummy(ImVec2(0,4));
}

// Click Trainer's own dedicated page, per Nigel: the rhythm bar wants more
// room than the ##jmfConstrain sidebar (42% width, shared with Trainer/
// Segments/Notes) can give it. Uses the tab's FULL content width instead --
// the JMF backdrop still renders behind it either way (drawJupiterBackdrop
// runs before drawTabContent, unconditionally), this is just about how much
// of the foreground we claim.
void MenuInterface::drawJupiterClickTrainerPage(){
    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    engine->jupiterClickBarPageVisible=true;

    // A keybind rebind left armed (clicked "rebind" on Settings > Keybinds,
    // then navigated away without pressing the target key or Escape) would
    // otherwise intercept the down-press of your first Space/Up/W here as
    // the rebind target, silently reassigning that keybind and leaving an
    // unpaired release mark behind it. Being on this page at all means any
    // such rebind attempt was abandoned, so clear it defensively.
    rebindTarget=nullptr;

    if(Widgets::StyledButton("<- Back",ImVec2(90,28),theme,anim)){
        jupiterClickBarPageOpen=false;
        gbju::stopClickBarMusic();
        engine->jupiterClickBarMyClicks.clear();
        engine->jupiterClickBarMyReleases.clear();
    }
    ImGui::Dummy(ImVec2(0,10));

    if(fontHeading)ImGui::PushFont(fontHeading);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
    ImGui::TextWrapped("Click Trainer");
    ImGui::PopStyleColor();
    if(fontHeading)ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Left edge of a block crossing the white line means click, right edge means release.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,12));

    if(Widgets::ToggleSwitch("Show Click Bar",&engine->jupiterClickBarEnabled,theme,anim))
        mod->setSavedValue("jupiter_clickbar_enabled",engine->jupiterClickBarEnabled);
    if(engine->jupiterClickBarEnabled){
        if(Widgets::StyledSliderFloat("Window (sec)",&engine->jupiterClickBarWindow,0.3f,4.f,theme))
            mod->setSavedValue("jupiter_clickbar_window",(double)engine->jupiterClickBarWindow);
        bool sliderJustReleased=ImGui::IsItemDeactivated(); // dragging this and releasing over the bar below shouldn't log a mark
        ImGui::Dummy(ImVec2(0,14));
        drawJupiterClickBar(theme,anim,engine,engine->jupiterClickBarWindow,sliderJustReleased,90.f);
    }

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Click Deviation",theme);
    {
        auto* pl=PlayLayer::get();
        if(engine->jupiterMacro.clickIntervalsSec.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("No click data yet.");
            ImGui::PopStyleColor();
        } else if(!pl||!pl->m_player1||engine->isPlaying()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Play the level yourself (not bot playback) to compare your clicks against the macro's.");
            ImGui::PopStyleColor();
        } else {
            bool holding=(bool)pl->m_player1->m_holdingButtons[1];
            if(holding&&!engine->jupiterDeviationHolding){
                double tps=engine->jupiterMacro.clickBarTps>0.0?engine->jupiterMacro.clickBarTps:240.0;
                double nowSec=(double)engine->updater.getFrame()/tps;
                double bestDelta=1e9;
                for(auto const& iv:engine->jupiterMacro.clickIntervalsSec){
                    double d=iv.first-nowSec;
                    if(std::fabs(d)<std::fabs(bestDelta))bestDelta=d;
                }
                if(bestDelta<1e8){
                    engine->jupiterLastDeviationFrames=-(int)std::lround(bestDelta*tps);
                    engine->jupiterHasDeviationReading=true;
                }
            }
            engine->jupiterDeviationHolding=holding;

            if(!engine->jupiterHasDeviationReading){
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::TextWrapped("Waiting for your first click...");
                ImGui::PopStyleColor();
            } else {
                int f=engine->jupiterLastDeviationFrames;
                const char* verdict=f==0?"on time":(f<0?"early":"late");
                ImVec4 col=f==0?ImVec4(0.3f,0.9f,0.4f,1.f):(std::abs(f)<=3?ImVec4(0.95f,0.85f,0.3f,1.f):ImVec4(0.95f,0.35f,0.35f,1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,col);
                ImGui::Text("Last click: %d frame%s %s",std::abs(f),std::abs(f)==1?"":"s",verdict);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Music",theme);
    if(Widgets::ToggleSwitch("Synced Level Music",&engine->jupiterMusicEnabled,theme,anim))
        mod->setSavedValue("jupiter_music_enabled",engine->jupiterMusicEnabled);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Plays while actually on Jupiter My Favourite, seeked to match your current frame -- frame 0 is song position 0, no offset, and it resyncs itself after respawns/restarts instead of just playing through once.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Ghosts & Scrub Preview",theme);
    if(Widgets::ToggleSwitch("Macro Ghost",&engine->jupiterGhostEnabled,theme,anim))
        mod->setSavedValue("jupiter_ghost_enabled",engine->jupiterGhostEnabled);
    if(Widgets::ToggleSwitch("Your Best-Attempt Ghost",&engine->jupiterBestGhostEnabled,theme,anim))
        mod->setSavedValue("jupiter_bestghost_enabled",engine->jupiterBestGhostEnabled);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Best-attempt ghost is session-only, not saved to disk, and only tracks real manual attempts, not bot playback. Both ghosts render in the game world, right on the player's actual path.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,10));

    Widgets::ToggleSwitch("Scrub Preview",&engine->jupiterScrubActive,theme,anim);
    if(engine->jupiterScrubActive){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Ghosts freeze at this position in the level instead of following live playback, so you can preview any point without touching the actual player -- not a real teleport (the checkpoint system that would need is the same fragile one flagged elsewhere in this tab).");
        ImGui::PopStyleColor();
        Widgets::StyledSliderFloat("Scrub Percent",&engine->jupiterScrubPercent,0.f,100.f,theme);
    }
}

void MenuInterface::drawJupiterTab(){
    if(jupiterClickBarPageOpen){drawJupiterClickTrainerPage();return;}

    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    static char jupiterNotesBuf[1024];
    static bool jupiterNotesInit=false;

        // Theme + backdrop are now applied once across the WHOLE menu window
    // for as long as this tab is active (see drawMainWindow/drawMegaHackWindow),
    // not just boxed into this tab's own content -- `theme` here already reads
    // as the Jupiter palette by the time this function runs.

    // Real content has to stay clear of the diagonal wave-ribbon backdrop
    // (per Nigel: "the features shouldnt go past the line"), so it's boxed
    // into a narrower child instead of using the full tab width.
    ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(0,0,0,0));
    // Width picked against the ribbon's own geometry, not guessed: the spine's
    // narrowest point is x=0.36 (at y=0.66, see drawJupiterWaveRibbon), so 0.42
    // was ALWAYS capable of overlapping it once sidebar content got tall enough
    // to reach that height -- which it now does, with Stats/Segment Looping/
    // Share added on top of Trainer/Segments/Notes. 0.32 leaves real margin.
    ImGui::BeginChild("##jmfConstrain",ImVec2(ImGui::GetContentRegionAvail().x*0.32f,-1),false);

    // Full name while the tab's actually open, per Nigel's sketch -- wraps to
    // more than one line rather than the short "JMF" used in the tab rail.
    if(fontHeading)ImGui::PushFont(fontHeading);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
    ImGui::TextWrapped("Nigel's Jupiter My Favourite Trainer");
    ImGui::PopStyleColor();
    if(fontHeading)ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,6));

    auto* pl=PlayLayer::get();
    std::string currentLevel = (pl&&pl->m_level) ? std::string(pl->m_level->m_levelName) : engine->loadedMacroLevelName;
    bool isJupiter=false;
    {
        std::string lower=currentLevel;
        std::transform(lower.begin(),lower.end(),lower.begin(),::tolower);
        isJupiter = lower.find("jupiter my favourite")!=std::string::npos;
    }

    if(isJupiter){
        Widgets::StatusBadge("ACTIVE",ImVec4(0.30f,0.88f,0.92f,1.f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped(currentLevel.empty()
            ? "No level loaded. Enter (or load a macro for) Jupiter my Favourite to activate the trainer."
            : ("Currently on \""+currentLevel+"\" -- this tab is scoped to Jupiter my Favourite specifically, but everything below still works on whatever's loaded.").c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,8));

    Widgets::SectionHeader("Stats",theme);
    ImGui::Text("Attempts this session: %d",engine->jupiterAttemptCount);
    ImGui::Text("Best this session: %.1f%%",engine->jupiterSessionBestPct);
    if(!engine->jupiterDeathPcts.empty()){
        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Death heatmap (%zu death%s logged this session)",
            engine->jupiterDeathPcts.size(),engine->jupiterDeathPcts.size()==1?"":"s");
        ImGui::PopStyleColor();
        ImVec2 hmPos=ImGui::GetCursorScreenPos();
        float hmW=ImGui::GetContentRegionAvail().x,hmH=18.f;
        ImDrawList* hmDl=ImGui::GetWindowDrawList();
        hmDl->AddRectFilled(hmPos,ImVec2(hmPos.x+hmW,hmPos.y+hmH),IM_COL32(30,26,60,255),3.f);
        // Bucket into 40 bins across 0-100% so repeated deaths at the same
        // spot visibly stack up as taller/brighter marks instead of just
        // overlapping into one indistinguishable line.
        const int bins=40;
        int counts[bins]={0};
        int maxCount=1;
        for(float p:engine->jupiterDeathPcts){
            int b=std::clamp((int)(p/100.f*bins),0,bins-1);
            counts[b]++;
            maxCount=std::max(maxCount,counts[b]);
        }
        for(int b=0;b<bins;b++){
            if(counts[b]==0)continue;
            float bx0=hmPos.x+hmW*((float)b/bins);
            float bx1=hmPos.x+hmW*((float)(b+1)/bins);
            float t=(float)counts[b]/(float)maxCount;
            ImU32 col=theme.getAccentU32(0.35f+0.65f*t);
            hmDl->AddRectFilled(ImVec2(bx0,hmPos.y+hmH*(1.f-t)),ImVec2(bx1,hmPos.y+hmH),col);
        }
        ImGui::Dummy(ImVec2(hmW,hmH+4));
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Trainer",theme);
    if(engine->replay.m_pathSamples.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("No path data yet for the loaded macro. Load your converted TCBot macro and let it play through once (bot or manual) -- ground truth gets captured automatically and saved when the level completes.");
        ImGui::PopStyleColor();
    } else {
        if(Widgets::ToggleSwitch("Show Path",&engine->showMacroPath,theme,anim))
            mod->setSavedValue("hack_show_macro_path",engine->showMacroPath);
        if(Widgets::ToggleSwitch("Progressive Reveal",&engine->trainerRevealEnabled,theme,anim))
            mod->setSavedValue("hack_trainer_reveal_enabled",engine->trainerRevealEnabled);
        if(engine->trainerRevealEnabled){
            if(Widgets::StyledSliderFloat("Reveal Buffer",&engine->trainerRevealBuffer,0.f,300.f,theme))
                mod->setSavedValue("hack_trainer_reveal_buffer",(double)engine->trainerRevealBuffer);
            ImGui::Text("Furthest reached: %.0f",engine->replay.m_trainerBestX);
            ImGui::SameLine();
            if(Widgets::StyledButton("Reset Progress##trainer",ImVec2(140,24),theme,anim)){
                engine->replay.m_trainerBestX=0.f;
                engine->replay.saveTrainerProgressNow();
            }
        }
        if(Widgets::StyledSliderFloat("Marker Size",&engine->macroPathMarkerSize,3.f,20.f,theme))
            mod->setSavedValue("hack_macro_path_marker_size",(double)engine->macroPathMarkerSize);
        if(Widgets::StyledSliderFloat("Line Opacity",&engine->macroPathLineOpacity,0.1f,1.f,theme))
            mod->setSavedValue("hack_macro_path_line_opacity",(double)engine->macroPathLineOpacity);
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Click Trainer",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Click/hold windows scrolling toward a fixed line at constant real-time speed. Its own page now -- too cramped squeezed in here.");
    ImGui::PopStyleColor();
    if(Widgets::StyledButton("Open Click Trainer ->",ImVec2(-1,32),theme,anim)){
        jupiterClickBarPageOpen=true;
        engine->jupiterClickBarPaused=true;
        engine->jupiterClickBarPosSec=0.0;
        engine->jupiterClickBarMyClicks.clear();
        engine->jupiterClickBarMyReleases.clear();
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Segments",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Personal landmarks -- name the hard parts so the reveal overlay means something at a glance. Doesn't jump you there, just labels a position for your own reference.");
    ImGui::PopStyleColor();

    static char segLabelBuf[64]="";
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##segLabel","segment name",segLabelBuf,sizeof(segLabelBuf));
    ImGui::SameLine();
    bool canMark = pl && pl->m_player1 && segLabelBuf[0];
    if(!canMark)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
    bool markClicked=Widgets::StyledButton("Mark Here",ImVec2(84,0),theme,anim);
    if(!canMark)ImGui::PopStyleVar();
    if(markClicked&&canMark){
        float x=pl->m_player1->m_position.x;
        if(!engine->jupiterSegmentsRaw.empty())engine->jupiterSegmentsRaw+=";";
        engine->jupiterSegmentsRaw += std::string(segLabelBuf)+","+std::to_string(x)+",";
        mod->setSavedValue("jupiter_segments",engine->jupiterSegmentsRaw);
        segLabelBuf[0]=0;
    }

    if(!engine->jupiterMacro.clickIntervalsSec.empty()&&!engine->jupiterMacro.pathSamples.empty()){
        if(Widgets::StyledButton("Suggest Segments (from click density)",ImVec2(-1,26),theme,anim)){
            auto suggestions=suggestSegmentsFromClickDensity(
                engine->jupiterMacro.clickIntervalsSec,engine->jupiterMacro.pathSamples,
                engine->jupiterMacro.clickBarTps,engine->jupiterSegmentsRaw);
            if(!suggestions.empty()){
                auto segs=parseJupiterSegments(engine->jupiterSegmentsRaw);
                for(auto& s:suggestions)segs.push_back(s);
                engine->jupiterSegmentsRaw=serializeJupiterSegments(segs);
                mod->setSavedValue("jupiter_segments",engine->jupiterSegmentsRaw);
            }
        }
    }

    {
        static int noteEditIdx=-1;
        static char noteBuf[128]="";
        auto segs=parseJupiterSegments(engine->jupiterSegmentsRaw);
        int removeIdx=-1;
        bool dirty=false;
        for(int i=0;i<(int)segs.size();i++){
            ImGui::PushID(i);
            ImGui::Text("%s",segs[i].label.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::Text("(x=%.0f)",segs[i].x);
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x-44);
            if(ImGui::SmallButton(noteEditIdx==i?"note v":"note >")){
                if(noteEditIdx==i)noteEditIdx=-1;
                else{noteEditIdx=i;snprintf(noteBuf,sizeof(noteBuf),"%s",segs[i].note.c_str());}
            }
            ImGui::SameLine();
            if(ImGui::SmallButton("x"))removeIdx=i;
            if(noteEditIdx==i){
                ImGui::SetNextItemWidth(-1);
                if(ImGui::InputTextWithHint("##segNote","note for this segment",noteBuf,sizeof(noteBuf))){
                    segs[i].note=noteBuf;
                    dirty=true;
                }
            } else if(!segs[i].note.empty()){
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::TextWrapped("  %s",segs[i].note.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        if(removeIdx>=0){
            segs.erase(segs.begin()+removeIdx);
            noteEditIdx=-1;
            dirty=true;
        }
        if(dirty){
            engine->jupiterSegmentsRaw=serializeJupiterSegments(segs);
            mod->setSavedValue("jupiter_segments",engine->jupiterSegmentsRaw);
        }
    }

    ImGui::Dummy(ImVec2(0,10));
    Widgets::SectionHeader("Segment Looping",theme);
    {
        auto segs=parseJupiterSegments(engine->jupiterSegmentsRaw);
        if(segs.size()<2){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Mark at least two segments to loop between them.");
            ImGui::PopStyleColor();
        } else {
            if(engine->jupiterLoopStartIdx>=(int)segs.size())engine->jupiterLoopStartIdx=-1;
            if(engine->jupiterLoopEndIdx>=(int)segs.size())engine->jupiterLoopEndIdx=-1;
            auto segCombo=[&](const char* id,int* idx){
                std::string preview=(*idx>=0&&*idx<(int)segs.size())?segs[*idx].label:"(none)";
                ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x-8)*0.5f);
                if(ImGui::BeginCombo(id,preview.c_str())){
                    for(int i=0;i<(int)segs.size();i++)
                        if(ImGui::Selectable(segs[i].label.c_str(),*idx==i))*idx=i;
                    ImGui::EndCombo();
                }
            };
            segCombo("##loopStart",&engine->jupiterLoopStartIdx);
            ImGui::SameLine();
            segCombo("##loopEnd",&engine->jupiterLoopEndIdx);
            bool validRange=engine->jupiterLoopStartIdx>=0&&engine->jupiterLoopEndIdx>=0&&
                segs[engine->jupiterLoopStartIdx].x<segs[engine->jupiterLoopEndIdx].x;
            if(!validRange)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
            if(Widgets::ToggleSwitch("Auto-Loop",&engine->jupiterLoopEnabled,theme,anim)&&!validRange)
                engine->jupiterLoopEnabled=false;
            if(!validRange)ImGui::PopStyleVar();
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped(!validRange
                ? "Pick a start and end segment (start must come before end) to arm the loop."
                : "The first time you reach the start segment, a real practice checkpoint gets placed there (pl->markCheckpoint() -- the same call your own checkpoint keybind makes, not a reconstructed one) -- dying anywhere after that respawns you there automatically, same as normal practice mode. Only places one per enable, so it won't pile up checkpoints or touch any you've placed yourself elsewhere.");
            ImGui::PopStyleColor();

            if(engine->jupiterLoopEnabled&&validRange&&pl&&pl->m_player1){
                static bool loopArmed=true;
                static bool checkpointPlaced=false;
                static int lastStartIdx=-1;
                if(lastStartIdx!=engine->jupiterLoopStartIdx){lastStartIdx=engine->jupiterLoopStartIdx;checkpointPlaced=false;}

                float startX=segs[engine->jupiterLoopStartIdx].x;
                float endX=segs[engine->jupiterLoopEndIdx].x;
                float px=pl->m_player1->m_position.x;
                if(px<startX+5.f){
                    loopArmed=true;
                } else {
                    if(!checkpointPlaced){
                        checkpointPlaced=true;
                        pl->markCheckpoint();
                        Notification::create("Loop checkpoint placed",NotificationIcon::Success)->show();
                    }
                    if(loopArmed&&px>=endX){
                        loopArmed=false;
                        Notification::create("Loop end reached",NotificationIcon::Success)->show();
                    }
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0,10));
    Widgets::SectionHeader("Share",theme);
    {
        static char importBuf[512]="";
        static std::string importErr;
        if(Widgets::StyledButton("Copy Export Code",ImVec2(-1,26),theme,anim)){
            ImGui::SetClipboardText(exportSegmentsCode(engine->jupiterSegmentsRaw,engine->jupiterNotes).c_str());
            Notification::create("Copied JMF code to clipboard",NotificationIcon::Success)->show();
        }
        ImGui::Dummy(ImVec2(0,4));
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint("##importCode","paste JMF code here",importBuf,sizeof(importBuf));
        ImGui::SameLine();
        if(Widgets::StyledButton("Import",ImVec2(80,0),theme,anim)){
            if(importSegmentsCode(importBuf,engine->jupiterSegmentsRaw,engine->jupiterNotes,importErr)){
                mod->setSavedValue("jupiter_segments",engine->jupiterSegmentsRaw);
                mod->setSavedValue("jupiter_notes",engine->jupiterNotes);
                jupiterNotesInit=false; // force the Notes textbox below to re-sync from the freshly-imported value
                importBuf[0]=0;
                Notification::create("Imported segments + notes",NotificationIcon::Success)->show();
            } else {
                Notification::create(importErr.c_str(),NotificationIcon::Error)->show();
            }
        }
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Notes",theme);
    if(!jupiterNotesInit){
        snprintf(jupiterNotesBuf,sizeof(jupiterNotesBuf),"%s",engine->jupiterNotes.c_str());
        jupiterNotesInit=true;
    }
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputTextMultiline("##jupiterNotes",jupiterNotesBuf,sizeof(jupiterNotesBuf),ImVec2(-1,100))){
        engine->jupiterNotes=jupiterNotesBuf;
        mod->setSavedValue("jupiter_notes",engine->jupiterNotes);
    }

    ImGui::Dummy(ImVec2(0,8));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Rehearsal mode (scrub playback, segment looping) isn't built yet -- that's the next pass. Click-rhythm cues are live above.");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// General Trainer tab's click bar -- structural duplicate of
// drawJupiterClickBar, reading trainer* fields instead of jupiter* ones. See
// trainerghost.hpp for why the ghost/music side of this feature is
// duplicated rather than shared; same reasoning applies here: this is a full
// interactive widget (mouse-hover rect, drag-skim) reading ~8 fields by
// name, and parameterizing it would mean changing an already-shipped,
// working function's signature for no real benefit over a clean copy.
static void drawTrainerClickBar(ThemeEngine& theme,AnimationState& anim,GucciEngine* engine,float windowSeconds,bool externalWidgetJustReleased,float h=46.f){
    auto& trn=engine->trainerMacro;
    if(trn.clickIntervalsSec.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("No click data yet.");
        ImGui::PopStyleColor();
        return;
    }

    double maxT=0.0;
    for(auto const& iv:trn.clickIntervalsSec)maxT=std::max(maxT,iv.second);
    double loopLen=std::max(maxT,1.0);

    double realNow=ImGui::GetTime();
    if(!engine->trainerClickBarPaused){
        double dt=realNow-engine->trainerClickBarLastRealTime;
        if(dt>0.0&&dt<1.0){
            double newPos=engine->trainerClickBarPosSec+dt;
            if(newPos>=loopLen){
                engine->trainerClickBarPosSec=0.0;
                if(engine->trainerClickBarLoop){
                    engine->trainerClickBarMyClicks.clear();
                    engine->trainerClickBarMyReleases.clear();
                } else {
                    engine->trainerClickBarPaused=true;
                }
            } else {
                engine->trainerClickBarPosSec=newPos;
            }
        }
    }
    engine->trainerClickBarLastRealTime=realNow;

    gbtr::syncTrainerClickBarMusic(true,engine->trainerClickBarPaused,engine->trainerClickBarPosSec);

    if(Widgets::StyledButton(engine->trainerClickBarPaused?"Resume":"Pause",ImVec2(80,24),theme,anim))
        engine->trainerClickBarPaused=!engine->trainerClickBarPaused;
    ImGui::SameLine();
    if(Widgets::StyledButton("Reset",ImVec2(70,24),theme,anim)){
        engine->trainerClickBarPosSec=0.0;
        engine->trainerClickBarMyClicks.clear();
        engine->trainerClickBarMyReleases.clear();
    }
    ImGui::SameLine();
    if(Widgets::ToggleSwitch("Loop",&engine->trainerClickBarLoop,theme,anim))
        Mod::get()->setSavedValue("trainer_clickbar_loop",engine->trainerClickBarLoop);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::Text("%.1fs / %.1fs",engine->trainerClickBarPosSec,loopLen);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,6));

    ImVec2 pos=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x;
    ImDrawList* dl=ImGui::GetWindowDrawList();

    bool mouseOverBar=ImGui::IsMouseHoveringRect(pos,ImVec2(pos.x+w,pos.y+h));
    bool blockMark=!mouseOverBar||externalWidgetJustReleased;
    if(!blockMark&&ImGui::IsMouseClicked(ImGuiMouseButton_Left))engine->trainerClickBarMyClicks.push_back(engine->trainerClickBarPosSec);
    if(!blockMark&&ImGui::IsMouseReleased(ImGuiMouseButton_Left))engine->trainerClickBarMyReleases.push_back(engine->trainerClickBarPosSec);

    const ImU32 barCol=IM_COL32(137,126,94,255);
    const ImU32 white=IM_COL32(255,255,255,255);
    const ImU32 clickCol=theme.getAccentU32(1.f);

    dl->AddRectFilled(pos,ImVec2(pos.x+w,pos.y+h),barCol,4.f);

    float centerX=pos.x+w*0.5f;
    float halfWindow=std::max(windowSeconds,0.2f)*0.5f;
    float pxPerSec=(w*0.5f)/halfWindow;
    double nowSec=engine->trainerClickBarPosSec;

    for(auto const& iv:trn.clickIntervalsSec){
        double relStart=iv.first-nowSec, relEnd=iv.second-nowSec;
        if(relEnd<-halfWindow||relStart>halfWindow)continue;
        float x0=centerX+(float)relStart*pxPerSec;
        float x1=centerX+(float)relEnd*pxPerSec;
        x0=std::max(x0,pos.x); x1=std::min(x1,pos.x+w);
        if(x1>x0)dl->AddRectFilled(ImVec2(x0,pos.y+5),ImVec2(x1,pos.y+h-5),clickCol,2.f);
    }

    auto drawMyMark=[&](double t,bool isRelease){
        double rel=t-nowSec;
        if(rel<-halfWindow||rel>halfWindow)return;
        float x=centerX+(float)rel*pxPerSec;
        float yMid=pos.y+h*0.5f;
        if(isRelease)dl->AddLine(ImVec2(x,pos.y+3),ImVec2(x,yMid),white,2.f);
        else dl->AddLine(ImVec2(x,yMid),ImVec2(x,pos.y+h-3),white,2.f);
    };
    for(double t:engine->trainerClickBarMyClicks)drawMyMark(t,false);
    for(double t:engine->trainerClickBarMyReleases)drawMyMark(t,true);

    dl->AddLine(ImVec2(centerX,pos.y-4),ImVec2(centerX,pos.y+h+4),white,3.f);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##trainerClickBarSkim",ImVec2(w,h));
    if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
        engine->trainerClickBarPaused=true;
        double posSec=engine->trainerClickBarPosSec-ImGui::GetIO().MouseDelta.x/pxPerSec;
        posSec=std::clamp(posSec,0.0,loopLen);
        engine->trainerClickBarPosSec=posSec;
    }
    ImGui::Dummy(ImVec2(0,4));
}

// Imported music for the Trainer tab: prompts a native file picker (first
// use of geode::utils::file::pick in this codebase -- it's an async,
// coroutine-based Task/Future API, unlike the synchronous openFolder used
// elsewhere here) and copies whatever's picked to a fixed on-disk location
// rather than referencing the original path live, so a later move/rename/
// delete of the source file can't silently break playback. listen()'s
// callback is documented as self-cleaning ("only be used in a global
// context"), so this deliberately isn't a member of MenuInterface -- it
// reaches into GucciEngine::get() itself instead of capturing `this`.
static geode::Task<bool> importTrainerMusicTask(){
    auto pickResult = co_await geode::utils::file::pick(
        geode::utils::file::PickMode::OpenFile,
        geode::utils::file::FilePickOptions{
            std::nullopt,
            { { "Audio Files", { "mp3" } } }
        }
    );
    if (pickResult.isErr()) co_return false;
    auto pathOpt = pickResult.unwrap();
    if (!pathOpt.has_value()) co_return false; // cancelled

    auto dest = Mod::get()->getSaveDir() / "trainer_music.mp3";
    std::error_code ec;
    std::filesystem::copy_file(*pathOpt, dest, std::filesystem::copy_options::overwrite_existing, ec);
    co_return !ec;
}
static void importTrainerMusic(){
    importTrainerMusicTask().listen([](bool* ok){
        auto* gb = GucciEngine::get();
        if (ok && *ok) {
            gb->trainerMusicImported = true;
            Mod::get()->setSavedValue("trainer_music_imported", true);
            Notification::create("Music imported", NotificationIcon::Success)->show();
        } else {
            Notification::create("Import failed or cancelled", NotificationIcon::Warning)->show();
        }
    });
}

void MenuInterface::drawTrainerClickTrainerPage(){
    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    engine->trainerClickBarPageVisible=true;

    // Same defensive clear as drawJupiterClickTrainerPage -- see its comment.
    rebindTarget=nullptr;

    if(Widgets::StyledButton("<- Back",ImVec2(90,28),theme,anim)){
        trainerClickBarPageOpen=false;
        gbtr::stopTrainerClickBarMusic();
        engine->trainerClickBarMyClicks.clear();
        engine->trainerClickBarMyReleases.clear();
    }
    ImGui::Dummy(ImVec2(0,10));

    if(fontHeading)ImGui::PushFont(fontHeading);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
    ImGui::TextWrapped("Click Trainer");
    ImGui::PopStyleColor();
    if(fontHeading)ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Left edge of a block crossing the white line means click, right edge means release.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,12));

    if(Widgets::ToggleSwitch("Show Click Bar",&engine->trainerClickBarEnabled,theme,anim))
        mod->setSavedValue("trainer_clickbar_enabled",engine->trainerClickBarEnabled);
    if(engine->trainerClickBarEnabled){
        if(Widgets::StyledSliderFloat("Window (sec)",&engine->trainerClickBarWindow,0.3f,4.f,theme))
            mod->setSavedValue("trainer_clickbar_window",(double)engine->trainerClickBarWindow);
        bool sliderJustReleased=ImGui::IsItemDeactivated();
        ImGui::Dummy(ImVec2(0,14));
        drawTrainerClickBar(theme,anim,engine,engine->trainerClickBarWindow,sliderJustReleased,90.f);
    }

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Click Deviation",theme);
    {
        auto* pl=PlayLayer::get();
        if(engine->trainerMacro.clickIntervalsSec.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("No click data yet.");
            ImGui::PopStyleColor();
        } else if(!pl||!pl->m_player1||engine->isPlaying()){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Play the level yourself (not bot playback) to compare your clicks against the macro's.");
            ImGui::PopStyleColor();
        } else {
            bool holding=(bool)pl->m_player1->m_holdingButtons[1];
            if(holding&&!engine->trainerDeviationHolding){
                double tps=engine->trainerMacro.clickBarTps>0.0?engine->trainerMacro.clickBarTps:240.0;
                double nowSec=(double)engine->updater.getFrame()/tps;
                double bestDelta=1e9;
                for(auto const& iv:engine->trainerMacro.clickIntervalsSec){
                    double d=iv.first-nowSec;
                    if(std::fabs(d)<std::fabs(bestDelta))bestDelta=d;
                }
                if(bestDelta<1e8){
                    engine->trainerLastDeviationFrames=-(int)std::lround(bestDelta*tps);
                    engine->trainerHasDeviationReading=true;
                }
            }
            engine->trainerDeviationHolding=holding;

            if(!engine->trainerHasDeviationReading){
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::TextWrapped("Waiting for your first click...");
                ImGui::PopStyleColor();
            } else {
                int f=engine->trainerLastDeviationFrames;
                const char* verdict=f==0?"on time":(f<0?"early":"late");
                ImVec4 col=f==0?ImVec4(0.3f,0.9f,0.4f,1.f):(std::abs(f)<=3?ImVec4(0.95f,0.85f,0.3f,1.f):ImVec4(0.95f,0.35f,0.35f,1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,col);
                ImGui::Text("Last click: %d frame%s %s",std::abs(f),std::abs(f)==1?"":"s",verdict);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Music",theme);
    if(Widgets::ToggleSwitch("Synced Music",&engine->trainerMusicEnabled,theme,anim))
        mod->setSavedValue("trainer_music_enabled",engine->trainerMusicEnabled);
    if(Widgets::StyledButton(engine->trainerMusicImported?"Re-Import Music":"Import Music",ImVec2(160,26),theme,anim))
        importTrainerMusic();
    if(engine->trainerMusicImported){
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextUnformatted("Music imported.");
        ImGui::PopStyleColor();
    }
    if(Widgets::StyledSliderFloat("Offset (sec)",&engine->trainerMusicOffsetSec,-3.f,3.f,theme))
        mod->setSavedValue("trainer_music_offset_sec",(double)engine->trainerMusicOffsetSec);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Plays while on the macro's level (or any level, if it has no recorded level name), seeked to match your current frame plus the offset above. Positive offset delays the music; negative brings it earlier.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0,18));
    Widgets::SectionHeader("Ghosts & Scrub Preview",theme);
    if(Widgets::ToggleSwitch("Macro Ghost",&engine->trainerGhostEnabled,theme,anim))
        mod->setSavedValue("trainer_ghost_enabled",engine->trainerGhostEnabled);
    if(Widgets::ToggleSwitch("Your Best-Attempt Ghost",&engine->trainerBestGhostEnabled,theme,anim))
        mod->setSavedValue("trainer_bestghost_enabled",engine->trainerBestGhostEnabled);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Best-attempt ghost is session-only, not saved to disk, and only tracks real manual attempts, not bot playback.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,10));

    Widgets::ToggleSwitch("Scrub Preview",&engine->trainerScrubActive,theme,anim);
    if(engine->trainerScrubActive){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Ghosts freeze at this position instead of following live playback.");
        ImGui::PopStyleColor();
        Widgets::StyledSliderFloat("Scrub Percent",&engine->trainerScrubPercent,0.f,100.f,theme);
    }
}

// General-purpose counterpart to drawJupiterTab: same toolset, but scoped to
// whichever of the user's own macros they've picked into trainerMacro
// instead of one bundled level. Deliberately plain full-width layout (no
// ##jmfConstrain-style narrow child, no wave-ribbon backdrop, no theme
// reskin) -- those only exist for JMF because activeTab==6 triggers a
// whole-window reskin in drawMainWindow/drawMegaHackWindow; a new tab at a
// new index doesn't trigger any of that, so this can look like every other
// ordinary tab.
void MenuInterface::drawTrainerTab(){
    if(trainerClickBarPageOpen){drawTrainerClickTrainerPage();return;}

    auto* engine=GucciEngine::get();
    auto* mod=Mod::get();
    static char trainerNotesBuf[1024];
    static bool trainerNotesInit=false;

    if(fontHeading)ImGui::PushFont(fontHeading);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.getAccent());
    ImGui::TextWrapped("Trainer");
    ImGui::PopStyleColor();
    if(fontHeading)ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Same toolset as the JMF tab -- Click Trainer, Ghosts, Segments, Stats, Music -- but for any of your own saved macros instead of one fixed level.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0,8));

    Widgets::SectionHeader("Macro",theme);
    if(engine->trainerMacro.loaded){
        ImGui::Text("Loaded: %s",engine->trainerMacroName.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("No macro loaded -- pick one below.");
        ImGui::PopStyleColor();
    }
    static char trainerMacroFilter[64]="";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##trainerMacroSearch","Search macros...",trainerMacroFilter,sizeof(trainerMacroFilter));
    auto matchesTrainerFilter=[&](const std::string& nm)->bool{
        if(trainerMacroFilter[0]==0)return true;
        std::string a=nm,b=trainerMacroFilter;
        std::transform(a.begin(),a.end(),a.begin(),::tolower);
        std::transform(b.begin(),b.end(),b.begin(),::tolower);
        return a.find(b)!=std::string::npos;
    };
    refreshReplayListIfNeeded(false);
    float trainerListH=std::max(80.f,std::min(160.f,(float)engine->storedMacros.size()*24.f+16.f));
    ImGui::BeginChild("##TrainerMacroList",ImVec2(-1,trainerListH),true);
    auto trainerMacroCopy=engine->storedMacros;
    if(trainerMacroCopy.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("No saved macros yet.");
        ImGui::PopStyleColor();
    }
    for(const auto& mn:trainerMacroCopy){
        if(!matchesTrainerFilter(mn))continue;
        bool isSel=(engine->trainerMacroName==mn && engine->trainerMacro.loaded);
        ImGui::PushID(mn.c_str());
        if(ImGui::Selectable(mn.c_str(),isSel)){
            if(engine->loadTrainerMacro(mn))
                Notification::create("Loaded macro into Trainer",NotificationIcon::Success)->show();
            else
                Notification::create("Couldn't load that macro",NotificationIcon::Error)->show();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if(!engine->incompatibleMacros.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("%zu macro(s) need converting first -- see the Macro tab's Saved Replays list.",engine->incompatibleMacros.size());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,8));

    auto* pl=PlayLayer::get();
    std::string currentLevel = (pl&&pl->m_level) ? std::string(pl->m_level->m_levelName) : "";
    bool levelKnown = !engine->trainerMacro.levelName.empty();
    if(!engine->trainerMacro.loaded){
        // nothing to show -- picker above already explains the state
    } else if(!levelKnown){
        Widgets::StatusBadge("ACTIVE (level unknown)",ImVec4(0.95f,0.75f,0.25f,1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("This macro has no recorded level name (common for imported/converted macros), so Stats/Ghost/Music stay active on any level instead of just one.");
        ImGui::PopStyleColor();
    } else if(gbtr::isTrainerLevel(pl)){
        Widgets::StatusBadge("ACTIVE",ImVec4(0.30f,0.88f,0.92f,1.f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped(currentLevel.empty()
            ? ("No level loaded. Enter \""+engine->trainerMacro.levelName+"\" to activate Stats/Ghost/Music for this macro.").c_str()
            : ("Currently on \""+currentLevel+"\" -- this macro is for \""+engine->trainerMacro.levelName+"\", but everything below still works on whatever's loaded.").c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0,8));

    Widgets::SectionHeader("Stats",theme);
    ImGui::Text("Attempts this session: %d",engine->trainerAttemptCount);
    ImGui::Text("Best this session: %.1f%%",engine->trainerSessionBestPct);
    if(!engine->trainerDeathPcts.empty()){
        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::Text("Death heatmap (%zu death%s logged this session)",
            engine->trainerDeathPcts.size(),engine->trainerDeathPcts.size()==1?"":"s");
        ImGui::PopStyleColor();
        ImVec2 hmPos=ImGui::GetCursorScreenPos();
        float hmW=ImGui::GetContentRegionAvail().x,hmH=18.f;
        ImDrawList* hmDl=ImGui::GetWindowDrawList();
        hmDl->AddRectFilled(hmPos,ImVec2(hmPos.x+hmW,hmPos.y+hmH),IM_COL32(30,26,60,255),3.f);
        const int bins=40;
        int counts[bins]={0};
        int maxCount=1;
        for(float p:engine->trainerDeathPcts){
            int b=std::clamp((int)(p/100.f*bins),0,bins-1);
            counts[b]++;
            maxCount=std::max(maxCount,counts[b]);
        }
        for(int b=0;b<bins;b++){
            if(counts[b]==0)continue;
            float bx0=hmPos.x+hmW*((float)b/bins);
            float bx1=hmPos.x+hmW*((float)(b+1)/bins);
            float t=(float)counts[b]/(float)maxCount;
            ImU32 col=theme.getAccentU32(0.35f+0.65f*t);
            hmDl->AddRectFilled(ImVec2(bx0,hmPos.y+hmH*(1.f-t)),ImVec2(bx1,hmPos.y+hmH),col);
        }
        ImGui::Dummy(ImVec2(hmW,hmH+4));
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Click Trainer",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Click/hold windows scrolling toward a fixed line at constant real-time speed.");
    ImGui::PopStyleColor();
    if(Widgets::StyledButton("Open Click Trainer ->",ImVec2(-1,32),theme,anim)){
        trainerClickBarPageOpen=true;
        engine->trainerClickBarPaused=true;
        engine->trainerClickBarPosSec=0.0;
        engine->trainerClickBarMyClicks.clear();
        engine->trainerClickBarMyReleases.clear();
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Segments",theme);
    ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
    ImGui::TextWrapped("Personal landmarks -- name the hard parts. Doesn't jump you there, just labels a position for your own reference.");
    ImGui::PopStyleColor();

    static char trainerSegLabelBuf[64]="";
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##trainerSegLabel","segment name",trainerSegLabelBuf,sizeof(trainerSegLabelBuf));
    ImGui::SameLine();
    bool canMarkT = pl && pl->m_player1 && trainerSegLabelBuf[0];
    if(!canMarkT)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
    bool markClickedT=Widgets::StyledButton("Mark Here",ImVec2(84,0),theme,anim);
    if(!canMarkT)ImGui::PopStyleVar();
    if(markClickedT&&canMarkT){
        float x=pl->m_player1->m_position.x;
        if(!engine->trainerSegmentsRaw.empty())engine->trainerSegmentsRaw+=";";
        engine->trainerSegmentsRaw += std::string(trainerSegLabelBuf)+","+std::to_string(x)+",";
        mod->setSavedValue("trainer_segments",engine->trainerSegmentsRaw);
        trainerSegLabelBuf[0]=0;
    }

    if(!engine->trainerMacro.clickIntervalsSec.empty()&&!engine->trainerMacro.pathSamples.empty()){
        if(Widgets::StyledButton("Suggest Segments (from click density)",ImVec2(-1,26),theme,anim)){
            auto suggestions=suggestSegmentsFromClickDensity(
                engine->trainerMacro.clickIntervalsSec,engine->trainerMacro.pathSamples,
                engine->trainerMacro.clickBarTps,engine->trainerSegmentsRaw);
            if(!suggestions.empty()){
                auto segs=parseJupiterSegments(engine->trainerSegmentsRaw);
                for(auto& s:suggestions)segs.push_back(s);
                engine->trainerSegmentsRaw=serializeJupiterSegments(segs);
                mod->setSavedValue("trainer_segments",engine->trainerSegmentsRaw);
            }
        }
    }

    {
        static int noteEditIdxT=-1;
        static char noteBufT[128]="";
        auto segs=parseJupiterSegments(engine->trainerSegmentsRaw);
        int removeIdx=-1;
        bool dirty=false;
        for(int i=0;i<(int)segs.size();i++){
            ImGui::PushID(i+5000);
            ImGui::Text("%s",segs[i].label.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::Text("(x=%.0f)",segs[i].x);
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x-44);
            if(ImGui::SmallButton(noteEditIdxT==i?"note v":"note >")){
                if(noteEditIdxT==i)noteEditIdxT=-1;
                else{noteEditIdxT=i;snprintf(noteBufT,sizeof(noteBufT),"%s",segs[i].note.c_str());}
            }
            ImGui::SameLine();
            if(ImGui::SmallButton("x"))removeIdx=i;
            if(noteEditIdxT==i){
                ImGui::SetNextItemWidth(-1);
                if(ImGui::InputTextWithHint("##trainerSegNote","note for this segment",noteBufT,sizeof(noteBufT))){
                    segs[i].note=noteBufT;
                    dirty=true;
                }
            } else if(!segs[i].note.empty()){
                ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
                ImGui::TextWrapped("  %s",segs[i].note.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        if(removeIdx>=0){
            segs.erase(segs.begin()+removeIdx);
            noteEditIdxT=-1;
            dirty=true;
        }
        if(dirty){
            engine->trainerSegmentsRaw=serializeJupiterSegments(segs);
            mod->setSavedValue("trainer_segments",engine->trainerSegmentsRaw);
        }
    }

    ImGui::Dummy(ImVec2(0,10));
    Widgets::SectionHeader("Segment Looping",theme);
    {
        auto segs=parseJupiterSegments(engine->trainerSegmentsRaw);
        if(segs.size()<2){
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("Mark at least two segments to loop between them.");
            ImGui::PopStyleColor();
        } else {
            if(engine->trainerLoopStartIdx>=(int)segs.size())engine->trainerLoopStartIdx=-1;
            if(engine->trainerLoopEndIdx>=(int)segs.size())engine->trainerLoopEndIdx=-1;
            auto segCombo=[&](const char* id,int* idx){
                std::string preview=(*idx>=0&&*idx<(int)segs.size())?segs[*idx].label:"(none)";
                ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x-8)*0.5f);
                if(ImGui::BeginCombo(id,preview.c_str())){
                    for(int i=0;i<(int)segs.size();i++)
                        if(ImGui::Selectable(segs[i].label.c_str(),*idx==i))*idx=i;
                    ImGui::EndCombo();
                }
            };
            segCombo("##trainerLoopStart",&engine->trainerLoopStartIdx);
            ImGui::SameLine();
            segCombo("##trainerLoopEnd",&engine->trainerLoopEndIdx);
            bool validRange=engine->trainerLoopStartIdx>=0&&engine->trainerLoopEndIdx>=0&&
                segs[engine->trainerLoopStartIdx].x<segs[engine->trainerLoopEndIdx].x;
            if(!validRange)ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.4f);
            if(Widgets::ToggleSwitch("Auto-Loop",&engine->trainerLoopEnabled,theme,anim)&&!validRange)
                engine->trainerLoopEnabled=false;
            if(!validRange)ImGui::PopStyleVar();
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped(!validRange
                ? "Pick a start and end segment (start must come before end) to arm the loop."
                : "The first time you reach the start segment, a real practice checkpoint gets placed there (pl->markCheckpoint() -- the same call your own checkpoint keybind makes, not a reconstructed one) -- dying anywhere after that respawns you there automatically. Only places one per enable.");
            ImGui::PopStyleColor();

            if(engine->trainerLoopEnabled&&validRange&&pl&&pl->m_player1){
                static bool loopArmedT=true;
                static bool checkpointPlacedT=false;
                static int lastStartIdxT=-1;
                if(lastStartIdxT!=engine->trainerLoopStartIdx){lastStartIdxT=engine->trainerLoopStartIdx;checkpointPlacedT=false;}

                float startX=segs[engine->trainerLoopStartIdx].x;
                float endX=segs[engine->trainerLoopEndIdx].x;
                float px=pl->m_player1->m_position.x;
                if(px<startX+5.f){
                    loopArmedT=true;
                } else {
                    if(!checkpointPlacedT){
                        checkpointPlacedT=true;
                        pl->markCheckpoint();
                        Notification::create("Loop checkpoint placed",NotificationIcon::Success)->show();
                    }
                    if(loopArmedT&&px>=endX){
                        loopArmedT=false;
                        Notification::create("Loop end reached",NotificationIcon::Success)->show();
                    }
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0,10));
    Widgets::SectionHeader("Share",theme);
    {
        static char importBufT[512]="";
        static std::string importErrT;
        if(Widgets::StyledButton("Copy Export Code",ImVec2(-1,26),theme,anim)){
            ImGui::SetClipboardText(exportSegmentsCode(engine->trainerSegmentsRaw,engine->trainerNotes).c_str());
            Notification::create("Copied Trainer code to clipboard",NotificationIcon::Success)->show();
        }
        ImGui::Dummy(ImVec2(0,4));
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint("##trainerImportCode","paste Trainer code here",importBufT,sizeof(importBufT));
        ImGui::SameLine();
        if(Widgets::StyledButton("Import",ImVec2(80,0),theme,anim)){
            if(importSegmentsCode(importBufT,engine->trainerSegmentsRaw,engine->trainerNotes,importErrT)){
                mod->setSavedValue("trainer_segments",engine->trainerSegmentsRaw);
                mod->setSavedValue("trainer_notes",engine->trainerNotes);
                trainerNotesInit=false;
                importBufT[0]=0;
                Notification::create("Imported segments + notes",NotificationIcon::Success)->show();
            } else {
                Notification::create(importErrT.c_str(),NotificationIcon::Error)->show();
            }
        }
    }

    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Notes",theme);
    if(!trainerNotesInit){
        snprintf(trainerNotesBuf,sizeof(trainerNotesBuf),"%s",engine->trainerNotes.c_str());
        trainerNotesInit=true;
    }
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputTextMultiline("##trainerNotes",trainerNotesBuf,sizeof(trainerNotesBuf),ImVec2(-1,100))){
        engine->trainerNotes=trainerNotesBuf;
        mod->setSavedValue("trainer_notes",engine->trainerNotes);
    }
}

void MenuInterface::drawCreditsTab(){
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0,8));
        {ImVec2 pos=ImGui::GetCursorScreenPos();
    float avail=ImGui::GetContentRegionAvail().x,heroH=90.f;
    dl->AddRectFilled(pos,ImVec2(pos.x+avail,pos.y+heroH),IM_COL32(14,10,2,255),8.f);
    dl->AddRect(pos,ImVec2(pos.x+avail,pos.y+heroH),theme.getAccentU32(0.55f),8.f,0,1.2f);
        auto diamond=[&](float cx,float cy,float r){
        dl->AddQuad(ImVec2(cx,cy-r),ImVec2(cx+r,cy),ImVec2(cx,cy+r),ImVec2(cx-r,cy),theme.getAccentU32(0.4f),0.8f);};
    diamond(pos.x+18,pos.y+heroH/2,10);diamond(pos.x+avail-18,pos.y+heroH/2,10);
    ImFont* bigF=fontTitle?fontTitle:(fontHeading?fontHeading:fontBody);
    if(bigF)ImGui::PushFont(bigF);
    ImVec2 ns=ImGui::CalcTextSize("Gucci Mane Fan");
    dl->AddText(bigF,bigF?bigF->FontSize:22.f,ImVec2(pos.x+(avail-ns.x)/2,pos.y+8),theme.getAccentU32(),"Gucci Mane Fan");
    if(bigF)ImGui::PopFont();
    if(fontSmall)ImGui::PushFont(fontSmall);
    dl->AddText(ImVec2(pos.x+(avail-ImGui::CalcTextSize("Concept, Direction & Testing").x)/2,pos.y+34),theme.getTextSecondaryU32(),"Concept, Direction & Testing");
    const char* badge=
        (activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)?"WR1 | Rapper | Never Covered":
        (activeTheme==THEME_JA)?"High Flyer | Ball Don't Lie | IYKYK":
        (activeTheme==THEME_GIDDEY)?"Australian | NBA | G'day Mate":
        (activeTheme==THEME_BAM)?"83 Pts | Center | BITCH IM KOBE":
        (activeTheme==THEME_SEXYY)?"Skee Yee | STL | Pound Town":
        (activeTheme==THEME_JUICE)?"Beta Tester | Bug Hunter | That's Tuff":
        (activeTheme==THEME_BUTLER)?"Playoff Jimmy | Big Face Coffee | Buckets":
        "Concept | Vision | Brrr";
    ImVec2 bs=ImGui::CalcTextSize(badge);
    float bx=pos.x+(avail-bs.x-16)/2,by=pos.y+52;
    dl->AddRectFilled(ImVec2(bx,by),ImVec2(bx+bs.x+16,by+18),theme.getAccentU32(0.12f),9.f);
    dl->AddRect(ImVec2(bx,by),ImVec2(bx+bs.x+16,by+18),theme.getAccentU32(0.4f),9.f,0,0.5f);
    dl->AddText(ImVec2(bx+8,by+2),theme.getAccentU32(),badge);
    if(fontSmall)ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,heroH+12));}
        struct{const char* init;const char* name;const char* role;}entries[]={
        {"N","guccimanefan (Nigelx1)","Concept, direction & testing"},
        {"C","Claude","Wrote the code. All of it. Not a euphemism."},
        {"K","kepe","yBot -- the file-size benchmark GBR6 was built to meet"},
        {"T","ToastexGD","Original ToastyReplay -- the GOAT"},
        {"G","Gucci Mane","He's the truth. Brrr."},
        {"T","Toosii","ToosiiBot theme & WR ambitions"},
        {"P","peony","Silicate dev -- dropped the source like Gucci drops albums. Brrr."},};
    for(auto& e:entries){
        ImVec2 pos=ImGui::GetCursorScreenPos();
        float avail=ImGui::GetContentRegionAvail().x,rowH=46.f;
        dl->AddRectFilled(pos,ImVec2(pos.x+avail,pos.y+rowH),theme.getCardU32(),7.f);
        dl->AddRect(pos,ImVec2(pos.x+avail,pos.y+rowH),theme.getAccentU32(0.1f),7.f,0,0.5f);
        float avR=16.f,avX=pos.x+22,avY=pos.y+rowH/2;
        dl->AddCircleFilled(ImVec2(avX,avY),avR,theme.getAccentU32(0.2f));
        dl->AddCircle(ImVec2(avX,avY),avR,theme.getAccentU32(0.4f),0,0.8f);
        char ini[2]={e.init[0],0};ImVec2 is=ImGui::CalcTextSize(ini);
        dl->AddText(ImVec2(avX-is.x/2,avY-is.y/2),theme.getAccentU32(),ini);
        if(fontBody)ImGui::PushFont(fontBody);
        dl->AddText(ImVec2(pos.x+46,pos.y+8),theme.getTextU32(),e.name);
        if(fontBody)ImGui::PopFont();
        if(fontSmall)ImGui::PushFont(fontSmall);
        dl->AddText(ImVec2(pos.x+46,pos.y+26),theme.getTextSecondaryU32(),e.role);
        if(fontSmall)ImGui::PopFont();
        ImGui::Dummy(ImVec2(0,rowH+5));}
        ImGui::Dummy(ImVec2(0,4));
    if(activeTheme==THEME_TOOSII||activeTheme==THEME_TOOSII_SYRACUSE||activeTheme==THEME_TOOSII_SACSTATE)
        Widgets::GucciQuote("\"Every click is a catch. I don't drop nothing. Not even frames.\"","-- Toosii, post-game presser",theme);
    else if(activeTheme==THEME_JA)
        Widgets::GucciQuote("\"Watch me. That's all I ask. Just watch.\"","-- Ja Morant",theme);
    else if(activeTheme==THEME_GIDDEY)
        Widgets::GucciQuote("\"I'm just happy to be here. Genuinely. This is a great game.\"","-- Josh Giddey",theme);
    else if(activeTheme==THEME_BAM)
        Widgets::GucciQuote("\"Every frame is a bucket. 83 of them. BITCH IM KOBE!!!\"","-- Bam Adebayo",theme);
    else if(activeTheme==THEME_SEXYY)
        Widgets::GucciQuote("\"Every click go stupid. Skee yee.\"","-- Sexyy Red",theme);
    else if(activeTheme==THEME_JUICE)
        Widgets::GucciQuote("\"I just wanted the frame windows to work. Then I got a whole theme.\"","-- Juice",theme);
    else if(activeTheme==THEME_BUTLER)
        Widgets::GucciQuote("\"Every frame's the playoffs to me. Brrr.\"","-- Jimmy Butler",theme);
    else
        Widgets::GucciQuote("\"I'm the foundation of all of this. Brrr.\"","-- Gucci Mane",theme);}
void MenuInterface::drawHudTab(){
    auto* engine=GucciEngine::get();
    Widgets::GucciQuote("\"Stats don't lie. Show me the numbers.\"","-- GucciBot v4.0",theme);
    ImGui::Dummy(ImVec2(0,4));
    Widgets::ToggleSwitch("Enable HUD",&engine->hud.enabled,theme,anim);
    if(!engine->hud.enabled){
        ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
        ImGui::TextWrapped("Enable to show live stats in-game.");
        ImGui::PopStyleColor();
        return;}
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Displayed Stats",theme);
    Widgets::ToggleSwitch("Tick / Frame",&engine->hud.showFrame,theme,anim);
    Widgets::ToggleSwitch("TPS",&engine->hud.showTPS,theme,anim);
    Widgets::ToggleSwitch("Player X",&engine->hud.showX,theme,anim);
    Widgets::ToggleSwitch("Player Y",&engine->hud.showY,theme,anim);
    Widgets::ToggleSwitch("X Velocity",&engine->hud.showXVel,theme,anim);
    Widgets::ToggleSwitch("Y Velocity",&engine->hud.showYVel,theme,anim);
    Widgets::ToggleSwitch("Rotation",&engine->hud.showRot,theme,anim);
    Widgets::ToggleSwitch("Bot State",&engine->hud.showState,theme,anim);
    ImGui::Dummy(ImVec2(0,8));
    Widgets::SectionHeader("Appearance",theme);
    const char* anchors[]={"Top Left","Top Right","Bottom Left","Bottom Right"};
    ImGui::Text("Position");ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##hudAnchor",&engine->hud.anchor,anchors,4);
    Widgets::ToggleSwitch("Big Font",&engine->hud.bigFont,theme,anim);
    Widgets::StyledSliderFloat("Scale",&engine->hud.scale,0.3f,2.0f,theme);
    Widgets::StyledSliderFloat("Opacity",&engine->hud.opacity,0.1f,1.0f,theme);}

static void saveColor(const char* pfx,const ImVec4& c){
    auto* m=Mod::get();
    m->setSavedValue(std::string(pfx)+"_r",c.x);m->setSavedValue(std::string(pfx)+"_g",c.y);
    m->setSavedValue(std::string(pfx)+"_b",c.z);m->setSavedValue(std::string(pfx)+"_a",c.w);}
static ImVec4 loadColor(const char* pfx,const ImVec4& def){
    auto* m=Mod::get();
    return ImVec4(m->getSavedValue<float>(std::string(pfx)+"_r",def.x),m->getSavedValue<float>(std::string(pfx)+"_g",def.y),
        m->getSavedValue<float>(std::string(pfx)+"_b",def.z),m->getSavedValue<float>(std::string(pfx)+"_a",def.w));}

void MenuInterface::saveSettings(){
    auto* mod=Mod::get();auto* eng=GucciEngine::get();
    saveColor("theme_accent",theme.accentColor);saveColor("theme_bg",theme.bgColor);
    saveColor("theme_card",theme.cardColor);saveColor("theme_text",theme.textPrimary);
    saveColor("theme_text2",theme.textSecondary);
    mod->setSavedValue("theme_bg_opacity",theme.bgOpacity);
    mod->setSavedValue("theme_corner_radius",theme.cornerRadius);
    mod->setSavedValue("theme_active_preset",theme.activePreset);
    mod->setSavedValue("theme_glow_cycle",theme.glowCycleEnabled);
    mod->setSavedValue("theme_glow_rate",theme.glowCycleRate);
    mod->setSavedValue("ambient_waves",ambientWavesEnabled);
    mod->setSavedValue("anim_speed",anim.animSpeed);
    mod->setSavedValue("anim_direction",(int)anim.openDirection);
    mod->setSavedValue("active_theme",(int)activeTheme);
    mod->setSavedValue("active_theme_preset",theme.activePreset);
    mod->setSavedValue("key_menu",keybinds.menu);
    mod->setSavedValue("key_frame_advance",keybinds.frameAdvance);
    mod->setSavedValue("key_frame_step",keybinds.frameStep);
    mod->setSavedValue("key_replay_toggle",keybinds.replayToggle);
    mod->setSavedValue("key_noclip",keybinds.noclip);
    mod->setSavedValue("key_safe_mode",keybinds.safeMode);
    mod->setSavedValue("key_trajectory",keybinds.trajectory);
    mod->setSavedValue("key_audio_pitch",keybinds.audioPitch);
    mod->setSavedValue("key_rng_lock",keybinds.rngLock);
    mod->setSavedValue("key_hitboxes",keybinds.hitboxes);
    mod->setSavedValue("key_layout_mode",keybinds.layoutMode);
    mod->setSavedValue("key_no_mirror",keybinds.noMirror);
    mod->setSavedValue("key_autoclicker",keybinds.autoclicker);
        mod->setSavedValue("key_intentional_death",keybinds.intentionalDeath);
    mod->setSavedValue("key_back_step",keybinds.backStep);
    mod->setSavedValue("key_auto_flip",keybinds.autoFlip);
    mod->setSavedValue("key_prevent_death",keybinds.preventDeath);
    mod->setSavedValue("key_mirror_inputs",keybinds.mirrorInputs);
    mod->setSavedValue("key_compact_mode",keybinds.compactMode);
    mod->setSavedValue("hack_hitboxes",eng->showHitboxes);
    mod->setSavedValue("hack_hitbox_death",eng->hitboxOnDeath);
    mod->setSavedValue("hack_hitbox_trail",eng->hitboxTrail);
    mod->setSavedValue("hack_hitbox_trail_len",eng->hitboxTrailLength);
    mod->setSavedValue("hack_trajectory",eng->pathPreview);
    mod->setSavedValue("hack_trajectory_len",eng->pathLength);
    mod->setSavedValue("hack_survival_indicator",eng->survivalIndicator);
    mod->setSavedValue("hack_survival_indicator_lookahead",eng->indicatorLookahead);
    mod->setSavedValue("hack_indicator_style",eng->indicatorStyle);
    mod->setSavedValue("hack_indicator_opacity",(double)eng->indicatorOpacity);
    mod->setSavedValue("hack_indicator_safe_r",(double)eng->indicatorSafeColorR);
    mod->setSavedValue("hack_indicator_safe_g",(double)eng->indicatorSafeColorG);
    mod->setSavedValue("hack_indicator_safe_b",(double)eng->indicatorSafeColorB);
    mod->setSavedValue("hack_indicator_danger_r",(double)eng->indicatorDangerColorR);
    mod->setSavedValue("hack_indicator_danger_g",(double)eng->indicatorDangerColorG);
    mod->setSavedValue("hack_indicator_danger_b",(double)eng->indicatorDangerColorB);
    mod->setSavedValue("hack_indicator_flash",eng->indicatorFlashEnabled);
    mod->setSavedValue("hack_indicator_sound",eng->indicatorSoundEnabled);
    mod->setSavedValue("hack_accuracy_hud",eng->accuracyHudEnabled);
    mod->setSavedValue("hack_show_macro_path",eng->showMacroPath);
    mod->setSavedValue("hack_macro_path_marker_size",(double)eng->macroPathMarkerSize);
    mod->setSavedValue("hack_macro_path_line_opacity",(double)eng->macroPathLineOpacity);
    mod->setSavedValue("hack_trainer_reveal_enabled",eng->trainerRevealEnabled);
    mod->setSavedValue("hack_trainer_reveal_buffer",(double)eng->trainerRevealBuffer);
    mod->setSavedValue("jupiter_notes",eng->jupiterNotes);
    mod->setSavedValue("jupiter_segments",eng->jupiterSegmentsRaw);
    mod->setSavedValue("jupiter_clickbar_enabled",eng->jupiterClickBarEnabled);
    mod->setSavedValue("jupiter_clickbar_window",(double)eng->jupiterClickBarWindow);
    mod->setSavedValue("jupiter_clickbar_loop",eng->jupiterClickBarLoop);
    mod->setSavedValue("jupiter_ghost_enabled",eng->jupiterGhostEnabled);
    mod->setSavedValue("jupiter_bestghost_enabled",eng->jupiterBestGhostEnabled);
    mod->setSavedValue("jupiter_music_enabled",eng->jupiterMusicEnabled);
    mod->setSavedValue("jupiter_music_offset_sec",(double)eng->jupiterMusicOffsetSec);
    mod->setSavedValue("trainer_macro_name",eng->trainerMacroName);
    mod->setSavedValue("trainer_notes",eng->trainerNotes);
    mod->setSavedValue("trainer_segments",eng->trainerSegmentsRaw);
    mod->setSavedValue("trainer_clickbar_enabled",eng->trainerClickBarEnabled);
    mod->setSavedValue("trainer_clickbar_window",(double)eng->trainerClickBarWindow);
    mod->setSavedValue("trainer_clickbar_loop",eng->trainerClickBarLoop);
    mod->setSavedValue("trainer_ghost_enabled",eng->trainerGhostEnabled);
    mod->setSavedValue("trainer_bestghost_enabled",eng->trainerBestGhostEnabled);
    mod->setSavedValue("trainer_music_enabled",eng->trainerMusicEnabled);
    mod->setSavedValue("trainer_music_offset_sec",(double)eng->trainerMusicOffsetSec);
    mod->setSavedValue("trainer_music_imported",eng->trainerMusicImported);
    mod->setSavedValue("hack_noclip",eng->noclipEnabled);
    mod->setSavedValue("hack_noclip_flash",eng->noclipDeathFlash);
    mod->setSavedValue("hack_noclip_color_r",eng->noclipDeathColorR);
    mod->setSavedValue("hack_noclip_color_g",eng->noclipDeathColorG);
    mod->setSavedValue("hack_noclip_color_b",eng->noclipDeathColorB);
    mod->setSavedValue("hack_noclipThreshold",(double)eng->noclipThreshold);
    mod->setSavedValue("hack_rng_lock",eng->rngLocked);
    mod->setSavedValue("hack_rng_seed",eng->rngSeedVal);
    mod->setSavedValue("hack_safe_mode",eng->protectedMode);
    mod->setSavedValue("hack_audio_pitch",eng->audioPitchEnabled);
    mod->setSavedValue("hack_no_mirror",eng->noMirrorEffect);
    mod->setSavedValue("hack_layout_mode",eng->layoutMode);
    mod->setSavedValue("hack_no_mirror_rec_only",eng->noMirrorRecordingOnly);
        mod->setSavedValue("feat_backwards_step",eng->updater.m_backwardsStepping);
    mod->setSavedValue("feat_back_step_count",eng->updater.m_maxBackstepFrames);
    mod->setSavedValue("feat_auto_flip",eng->updater.m_autoFlipOnDeath);
    mod->setSavedValue("feat_prevent_death",eng->updater.m_preventDeath);
    mod->setSavedValue("feat_mirror_inputs",eng->replay.m_mirrorInputs);
    mod->setSavedValue("feat_mirror_inverted",eng->replay.m_mirrorInverted);
    mod->setSavedValue("feat_maintain_gravity",eng->replay.m_maintainGravity);
    mod->setSavedValue("feat_autosave_end",eng->autosaveAtLevelEnd);
    mod->setSavedValue("feat_autosave_interval",eng->autosaveAtInterval);
    mod->setSavedValue("feat_autosave_interval_sec", eng->autosaveIntervalSec);
    mod->setSavedValue("feat_replay_backups",eng->replayBackupsEnabled);
    mod->setSavedValue("feat_scroll_speed_fix",eng->updater.m_ssbFix);
    mod->setSavedValue("feat_lock_delta",eng->updater.m_lockDelta);
    mod->setSavedValue("feat_frame_extrapolation",eng->updater.m_extrapolateFrames);
    mod->setSavedValue("hud_enabled",eng->hud.enabled);
    mod->setSavedValue("hud_show_frame",eng->hud.showFrame);
    mod->setSavedValue("hud_show_tps",eng->hud.showTPS);
    mod->setSavedValue("hud_show_x",eng->hud.showX);
    mod->setSavedValue("hud_show_y",eng->hud.showY);
    mod->setSavedValue("hud_show_xvel",eng->hud.showXVel);
    mod->setSavedValue("hud_show_yvel",eng->hud.showYVel);
    mod->setSavedValue("hud_show_rot",eng->hud.showRot);
    mod->setSavedValue("hud_show_state",eng->hud.showState);
    mod->setSavedValue("hud_anchor",eng->hud.anchor);
    mod->setSavedValue("hud_big_font",eng->hud.bigFont);
    mod->setSavedValue("hud_opacity",eng->hud.opacity);
    mod->setSavedValue("hud_scale",eng->hud.scale);

    mod->setSavedValue("hack_auto_retry",eng->hackAutoRetry);
    mod->setSavedValue("render_audio_codec",std::string(renderAudioCodecBuf));
    mod->setSavedValue("render_audio_bitrate",std::string(renderAudioBitrateBuf));
    auto* ac=Autoclicker::get();
    mod->setSavedValue("ac_enabled",ac->enabled);mod->setSavedValue("ac_player1",ac->player1);
    mod->setSavedValue("ac_player2",ac->player2);mod->setSavedValue("ac_hold_ticks",ac->holdTicks);
    mod->setSavedValue("ac_release_ticks",ac->releaseTicks);mod->setSavedValue("ac_only_holding",ac->onlyWhileHolding);

    mod->setSavedValue("eng_tick_rate",(float)eng->updater.m_tps);
    mod->setSavedValue("eng_speed",(float)eng->updater.m_speedhack);
        mod->setSavedValue("render_name",std::string(renderNameBuf));
    mod->setSavedValue("render_width",(int64_t)std::atoi(renderWidthBuf));
    mod->setSavedValue("render_height",(int64_t)std::atoi(renderHeightBuf));
    mod->setSavedValue("render_fps",(int64_t)std::atoi(renderFpsBuf));
    mod->setSavedValue("render_codec",std::string(renderCodecBuf));
    mod->setSavedValue("render_bitrate",std::string(renderBitrateBuf));
    mod->setSavedValue("render_file_extension",std::string(renderExtBuf));
    mod->setSavedValue("render_args",std::string(renderArgsBuf));
    mod->setSavedValue("render_pix_fmt",std::string(renderPixFmtBuf));
    mod->setSavedValue("render_video_args",std::string(renderVideoArgsBuf));
    mod->setSavedValue("render_audio_args",std::string(renderAudioArgsBuf));
    mod->setSavedValue("render_seconds_after",std::string(renderSecondsAfterBuf));
    mod->setSavedValue("render_include_audio",renderIncludeAudio);
    mod->setSavedValue("render_include_clicks",renderIncludeClicks);
    mod->setSavedValue("render_sfx_volume",(double)renderSfxVol);
    mod->setSavedValue("render_music_volume",(double)renderMusicVol);
    mod->setSavedValue("render_hide_endscreen",renderHideEndscreen);
    mod->setSavedValue("render_hide_levelcomplete",renderHideLevelComplete);
        auto* csm=ClickSoundManager::get();
    mod->setSavedValue("click_enabled",csm->enabled);
    mod->setSavedValue("click_pack",csm->activePackName);
    mod->setSavedValue("click_hard_vol",(double)csm->p1Pack.hardVolume);
    mod->setSavedValue("click_soft_vol",(double)csm->p1Pack.softVolume);
    mod->setSavedValue("click_release_vol",(double)csm->p1Pack.releaseVolume);
    mod->setSavedValue("click_softness",(double)csm->softness);
    mod->setSavedValue("click_delay_min",(double)csm->clickDelayMin);
    mod->setSavedValue("click_delay_max",(double)csm->clickDelayMax);
    mod->setSavedValue("click_play_during_playback",csm->playDuringPlayback);
    mod->setSavedValue("click_separate_p2",csm->separateP2Clicks);
    mod->setSavedValue("click_bg_noise",csm->backgroundNoiseEnabled);
    mod->setSavedValue("click_bg_noise_vol",(double)csm->backgroundNoiseVolume);
    mod->setSavedValue("window_size_w",windowSize.x);
    mod->setSavedValue("window_size_h",windowSize.y);
    mod->setSavedValue("main_sub_tab",mainSubTab);}

void MenuInterface::loadRenderSettings(){
    auto* mod=Mod::get();
    auto rn=mod->getSavedValue<std::string>("render_name","");
    auto rw=loadSV<int64_t>(mod,"render_width",1920);
    auto rh=loadSV<int64_t>(mod,"render_height",1080);
    auto rf=loadSV<int64_t>(mod,"render_fps",60);
    auto rc=loadSV<std::string>(mod,"render_codec","");
    auto rb=loadSV<std::string>(mod,"render_bitrate","30");
    auto re=loadSV<std::string>(mod,"render_file_extension",".mp4");
    auto ra=loadSV<std::string>(mod,"render_args","-pix_fmt yuv420p");
    auto rpf=loadSV<std::string>(mod,"render_pix_fmt","yuv420p");
    auto rv=loadSV<std::string>(mod,"render_video_args","colorspace=all=bt709:iall=bt470bg:fast=1");
    auto raa=loadSV<std::string>(mod,"render_audio_args","");
    auto rs=loadSV<std::string>(mod,"render_seconds_after","3");
    renderIncludeAudio=loadSV<bool>(mod,"render_include_audio",true);
    renderColorFix=loadSV<bool>(mod,"render_color_fix",true);
    renderIncludeClicks=loadSV<bool>(mod,"render_include_clicks",false);
    renderSfxVol=(float)loadSV<double>(mod,"render_sfx_volume",1.0);
    renderMusicVol=(float)loadSV<double>(mod,"render_music_volume",1.0);
    renderHideEndscreen=loadSV<bool>(mod,"render_hide_endscreen",false);
    renderHideLevelComplete=loadSV<bool>(mod,"render_hide_levelcomplete",false);
    snprintf(renderNameBuf,sizeof(renderNameBuf),"%s",rn.c_str());
    snprintf(renderWidthBuf,sizeof(renderWidthBuf),"%lld",rw);
    snprintf(renderHeightBuf,sizeof(renderHeightBuf),"%lld",rh);
    snprintf(renderFpsBuf,sizeof(renderFpsBuf),"%lld",rf);
    snprintf(renderCodecBuf,sizeof(renderCodecBuf),"%s",rc.c_str());
    snprintf(renderBitrateBuf,sizeof(renderBitrateBuf),"%s",rb.c_str());
    snprintf(renderExtBuf,sizeof(renderExtBuf),"%s",re.c_str());
    snprintf(renderArgsBuf,sizeof(renderArgsBuf),"%s",ra.c_str());
    snprintf(renderPixFmtBuf,sizeof(renderPixFmtBuf),"%s",rpf.c_str());
    snprintf(renderVideoArgsBuf,sizeof(renderVideoArgsBuf),"%s",rv.c_str());
    snprintf(renderAudioArgsBuf,sizeof(renderAudioArgsBuf),"%s",raa.c_str());
    snprintf(renderSecondsAfterBuf,sizeof(renderSecondsAfterBuf),"%s",rs.c_str());
    renderBufsInit=true;
        auto rFolder=mod->getSavedValue<std::string>("render_output_folder","");
    snprintf(outputFolderBuf,sizeof(outputFolderBuf),"%s",rFolder.c_str());}

void MenuInterface::loadSettings(){
    auto* mod=Mod::get();auto* eng=GucciEngine::get();
    ImVec4 accDef(0.788f,0.659f,0.298f,1.f),bgDef(0.051f,0.051f,0.051f,0.96f);
    ImVec4 cardDef(0.078f,0.078f,0.078f,1.f),txtDef(0.941f,0.910f,0.816f,1.f);
    ImVec4 txt2Def(0.478f,0.447f,0.376f,1.f);
    theme.accentColor=sanitizeColor(loadColor("theme_accent",accDef),accDef);
    theme.bgColor=sanitizeColor(loadColor("theme_bg",bgDef),bgDef);
    theme.cardColor=sanitizeColor(loadColor("theme_card",cardDef),cardDef);
    theme.textPrimary=sanitizeColor(loadColor("theme_text",txtDef),txtDef);
    theme.textSecondary=sanitizeColor(loadColor("theme_text2",txt2Def),txt2Def);
    theme.bgOpacity=sanitizeClamped(mod->getSavedValue<float>("theme_bg_opacity",0.96f),0.5f,1.f,0.96f);
    theme.cornerRadius=sanitizeClamped(mod->getSavedValue<float>("theme_corner_radius",5.f),0.f,16.f,5.f);
    theme.activePreset=std::clamp(mod->getSavedValue<int>("theme_active_preset",0),0,9);
    theme.glowCycleEnabled=mod->getSavedValue<bool>("theme_glow_cycle",false);
    theme.glowCycleRate=sanitizeClamped(mod->getSavedValue<float>("theme_glow_rate",0.5f),0.02f,1.f,0.5f);
    ambientWavesEnabled=mod->getSavedValue<bool>("ambient_waves",true);
    anim.animSpeed=sanitizeClamped(mod->getSavedValue<float>("anim_speed",8.f),2.f,24.f,8.f);
    anim.openDirection=(AnimDirection)mod->getSavedValue<int>("anim_direction",0);
    activeTheme=(BotTheme)std::clamp(mod->getSavedValue<int>("active_theme",(int)THEME_GUCCI),0,9);
    keybinds.menu=mod->getSavedValue<int>("key_menu",0xA4);
    keybinds.frameAdvance=mod->getSavedValue<int>("key_frame_advance",0x56);
    keybinds.frameStep=mod->getSavedValue<int>("key_frame_step",0x43);
    keybinds.replayToggle=mod->getSavedValue<int>("key_replay_toggle",0);
    keybinds.noclip=mod->getSavedValue<int>("key_noclip",0);
    keybinds.safeMode=mod->getSavedValue<int>("key_safe_mode",0);
    keybinds.trajectory=mod->getSavedValue<int>("key_trajectory",0);
    keybinds.audioPitch=mod->getSavedValue<int>("key_audio_pitch",0);
    keybinds.rngLock=mod->getSavedValue<int>("key_rng_lock",0);
    keybinds.hitboxes=mod->getSavedValue<int>("key_hitboxes",0);
    keybinds.layoutMode=mod->getSavedValue<int>("key_layout_mode",0);
    keybinds.noMirror=mod->getSavedValue<int>("key_no_mirror",0);
    keybinds.autoclicker=mod->getSavedValue<int>("key_autoclicker",0);
    eng->showHitboxes=mod->getSavedValue<bool>("hack_hitboxes",false);
    eng->hitboxOnDeath=mod->getSavedValue<bool>("hack_hitbox_death",false);
    eng->hitboxTrail=mod->getSavedValue<bool>("hack_hitbox_trail",false);
    eng->hitboxTrailLength=mod->getSavedValue<int>("hack_hitbox_trail_len",240);
    eng->pathPreview=mod->getSavedValue<bool>("hack_trajectory",false);
    eng->pathLength=mod->getSavedValue<int>("hack_trajectory_len",312);
    eng->survivalIndicator=mod->getSavedValue<bool>("hack_survival_indicator",false);
    eng->indicatorLookahead=mod->getSavedValue<int>("hack_survival_indicator_lookahead",20);
    eng->indicatorStyle=mod->getSavedValue<int>("hack_indicator_style",0);
    eng->indicatorOpacity=mod->getSavedValue<float>("hack_indicator_opacity",0.9f);
    eng->indicatorSafeColorR=mod->getSavedValue<float>("hack_indicator_safe_r",0.25f);
    eng->indicatorSafeColorG=mod->getSavedValue<float>("hack_indicator_safe_g",0.95f);
    eng->indicatorSafeColorB=mod->getSavedValue<float>("hack_indicator_safe_b",0.35f);
    eng->indicatorDangerColorR=mod->getSavedValue<float>("hack_indicator_danger_r",0.95f);
    eng->indicatorDangerColorG=mod->getSavedValue<float>("hack_indicator_danger_g",0.25f);
    eng->indicatorDangerColorB=mod->getSavedValue<float>("hack_indicator_danger_b",0.25f);
    eng->indicatorFlashEnabled=mod->getSavedValue<bool>("hack_indicator_flash",true);
    eng->indicatorSoundEnabled=mod->getSavedValue<bool>("hack_indicator_sound",false);
    eng->accuracyHudEnabled=mod->getSavedValue<bool>("hack_accuracy_hud",false);
    eng->showMacroPath=mod->getSavedValue<bool>("hack_show_macro_path",false);
    eng->macroPathMarkerSize=mod->getSavedValue<float>("hack_macro_path_marker_size",8.f);
    eng->macroPathLineOpacity=mod->getSavedValue<float>("hack_macro_path_line_opacity",0.6f);
    eng->trainerRevealEnabled=mod->getSavedValue<bool>("hack_trainer_reveal_enabled",true);
    eng->trainerRevealBuffer=mod->getSavedValue<float>("hack_trainer_reveal_buffer",40.f);
    eng->jupiterNotes=mod->getSavedValue<std::string>("jupiter_notes","");
    eng->jupiterSegmentsRaw=mod->getSavedValue<std::string>("jupiter_segments","");
    eng->jupiterClickBarEnabled=mod->getSavedValue<bool>("jupiter_clickbar_enabled",true);
    eng->jupiterClickBarWindow=mod->getSavedValue<float>("jupiter_clickbar_window",2.f);
    eng->jupiterClickBarLoop=mod->getSavedValue<bool>("jupiter_clickbar_loop",false);
    eng->jupiterGhostEnabled=mod->getSavedValue<bool>("jupiter_ghost_enabled",true);
    eng->jupiterBestGhostEnabled=mod->getSavedValue<bool>("jupiter_bestghost_enabled",false);
    eng->jupiterMusicEnabled=mod->getSavedValue<bool>("jupiter_music_enabled",true);
    eng->jupiterMusicOffsetSec=mod->getSavedValue<float>("jupiter_music_offset_sec",0.f);
    eng->trainerNotes=mod->getSavedValue<std::string>("trainer_notes","");
    eng->trainerSegmentsRaw=mod->getSavedValue<std::string>("trainer_segments","");
    eng->trainerClickBarEnabled=mod->getSavedValue<bool>("trainer_clickbar_enabled",true);
    eng->trainerClickBarWindow=mod->getSavedValue<float>("trainer_clickbar_window",2.f);
    eng->trainerClickBarLoop=mod->getSavedValue<bool>("trainer_clickbar_loop",false);
    eng->trainerGhostEnabled=mod->getSavedValue<bool>("trainer_ghost_enabled",true);
    eng->trainerBestGhostEnabled=mod->getSavedValue<bool>("trainer_bestghost_enabled",false);
    eng->trainerMusicEnabled=mod->getSavedValue<bool>("trainer_music_enabled",false);
    eng->trainerMusicOffsetSec=mod->getSavedValue<float>("trainer_music_offset_sec",0.f);
    eng->trainerMusicImported=mod->getSavedValue<bool>("trainer_music_imported",false);
    // Reconnect the remembered pick, if any -- no-op/false if the file's gone
    // since. Done here (general settings load), not GucciEngine::initialize(),
    // since that's reserved for the one-time bundled-Jupiter bootstrap and
    // engine-wide settings -- the most fragile part of this codebase per
    // CLAUDE.md, deliberately left untouched by this feature.
    {
        std::string savedTrainerMacro=mod->getSavedValue<std::string>("trainer_macro_name","");
        if(!savedTrainerMacro.empty())eng->loadTrainerMacro(savedTrainerMacro);
    }
    eng->noclipEnabled=mod->getSavedValue<bool>("hack_noclip",false);
    eng->noclipDeathFlash=mod->getSavedValue<bool>("hack_noclip_flash",true);
    eng->noclipDeathColorR=mod->getSavedValue<float>("hack_noclip_color_r",1.f);
    eng->noclipDeathColorG=mod->getSavedValue<float>("hack_noclip_color_g",0.f);
    eng->noclipDeathColorB=mod->getSavedValue<float>("hack_noclip_color_b",0.f);
    eng->noclipThreshold=mod->getSavedValue<float>("hack_noclipThreshold",0.f);
    eng->rngLocked=mod->getSavedValue<bool>("hack_rng_lock",false);
    eng->rngSeedVal=mod->getSavedValue<int>("hack_rng_seed",1);
    eng->protectedMode=mod->getSavedValue<bool>("hack_safe_mode",false);
    eng->audioPitchEnabled=mod->getSavedValue<bool>("hack_audio_pitch",true);
    eng->noMirrorEffect=mod->getSavedValue<bool>("hack_no_mirror",false);
    eng->layoutMode=mod->getSavedValue<bool>("hack_layout_mode",false);
    eng->noMirrorRecordingOnly=mod->getSavedValue<bool>("hack_no_mirror_rec_only",false);

    eng->hackAutoRetry=mod->getSavedValue<bool>("hack_auto_retry",false);
    snprintf(renderAudioCodecBuf,sizeof(renderAudioCodecBuf),"%s",mod->getSavedValue<std::string>("render_audio_codec","aac").c_str());
    snprintf(renderAudioBitrateBuf,sizeof(renderAudioBitrateBuf),"%s",mod->getSavedValue<std::string>("render_audio_bitrate","192k").c_str());
    auto* ac=Autoclicker::get();
    ac->enabled=mod->getSavedValue<bool>("ac_enabled",false);
    ac->player1=mod->getSavedValue<bool>("ac_player1",true);
    ac->player2=mod->getSavedValue<bool>("ac_player2",false);
    ac->holdTicks=mod->getSavedValue<int>("ac_hold_ticks",1);
    ac->releaseTicks=mod->getSavedValue<int>("ac_release_ticks",1);
    ac->onlyWhileHolding=mod->getSavedValue<bool>("ac_only_holding",false);

    eng->updater.m_tps=mod->getSavedValue<float>("eng_tick_rate",240.f);
    eng->updater.m_speedhack=mod->getSavedValue<float>("eng_speed",1.f);
        tempTickRate=(float)eng->updater.m_tps;tempGameSpeed=(float)eng->updater.m_speedhack;
    compactTempTickRate=tempTickRate;compactTempGameSpeed=tempGameSpeed;
    auto* csm=ClickSoundManager::get();
    csm->enabled=mod->getSavedValue<bool>("click_enabled",false);
    csm->activePackName=mod->getSavedValue<std::string>("click_pack","");
    csm->p1Pack.hardVolume=(float)mod->getSavedValue<double>("click_hard_vol",1.0);
    csm->p1Pack.softVolume=(float)mod->getSavedValue<double>("click_soft_vol",0.5);
    csm->p1Pack.releaseVolume=(float)mod->getSavedValue<double>("click_release_vol",0.8);
    csm->softness=(float)mod->getSavedValue<double>("click_softness",0.5);
    csm->clickDelayMin=(float)mod->getSavedValue<double>("click_delay_min",0.0);
    csm->clickDelayMax=(float)mod->getSavedValue<double>("click_delay_max",0.0);
    csm->playDuringPlayback=mod->getSavedValue<bool>("click_play_during_playback",true);
    csm->separateP2Clicks=mod->getSavedValue<bool>("click_separate_p2",false);
    csm->backgroundNoiseEnabled=mod->getSavedValue<bool>("click_bg_noise",false);
    csm->backgroundNoiseVolume=(float)mod->getSavedValue<double>("click_bg_noise_vol",0.5);
    windowSize.x=mod->getSavedValue<float>("window_size_w",580.f);
    windowSize.y=mod->getSavedValue<float>("window_size_h",540.f);
    mainSubTab=mod->getSavedValue<int>("main_sub_tab",0);
        keybinds.intentionalDeath=mod->getSavedValue<int>("key_intentional_death",0);
    keybinds.backStep=mod->getSavedValue<int>("key_back_step",0);
    keybinds.autoFlip=mod->getSavedValue<int>("key_auto_flip",0);
    keybinds.preventDeath=mod->getSavedValue<int>("key_prevent_death",0);
    keybinds.mirrorInputs=mod->getSavedValue<int>("key_mirror_inputs",0);
    keybinds.compactMode=mod->getSavedValue<int>("key_compact_mode",0);
        eng->updater.m_backwardsStepping=mod->getSavedValue<bool>("feat_backwards_step",false);
    eng->fwSweepRange=mod->getSavedValue<int>("fw_sweeprange",12);
    if(eng->fwMaxWindow > 2*eng->fwSweepRange) eng->fwMaxWindow = 2*eng->fwSweepRange;
    eng->fwSlackWindow=mod->getSavedValue<int>("fw_slackwindow",3);
    eng->fwFullRangeSweep=mod->getSavedValue<bool>("fw_full_range_sweep",false);
    eng->fwMaxFramesMeasured=mod->getSavedValue<int>("fw_maxframes",240);
    eng->fwSimSpeed=mod->getSavedValue<int>("fw_simspeed",1);
    eng->fwTestShipReleases=mod->getSavedValue<bool>("fw_test_ship_releases",true);
    eng->fwOrbAwareReleaseSkip=mod->getSavedValue<bool>("fw_orb_aware_release_skip",true);
    eng->fwLegendEnabled=mod->getSavedValue<bool>("fw_legend",false);
    eng->fwDebugMode=mod->getSavedValue<bool>("fw_debug_mode",false);
    eng->fwDebugSlowdown=mod->getSavedValue<int>("fw_debug_slowdown",30);
    eng->fwDelayMarkerCapture=mod->getSavedValue<bool>("fw_delay_marker_capture",false);
    eng->updater.m_logFrameIncrements=mod->getSavedValue<bool>("diag_log_frame_increments",false);
    eng->updater.m_maxBackstepFrames=mod->getSavedValue<int>("feat_back_step_count",120);
    eng->updater.m_autoFlipOnDeath=mod->getSavedValue<bool>("feat_auto_flip",false);
    eng->updater.m_preventDeath=mod->getSavedValue<bool>("feat_prevent_death",false);
    eng->replay.m_mirrorInputs=mod->getSavedValue<bool>("feat_mirror_inputs",false);
    eng->replay.m_mirrorInverted=mod->getSavedValue<bool>("feat_mirror_inverted",false);
    eng->replay.m_maintainGravity=mod->getSavedValue<bool>("feat_maintain_gravity",false);
    eng->autosaveAtLevelEnd=mod->getSavedValue<bool>("feat_autosave_end",true);
    eng->autosaveAtInterval=mod->getSavedValue<bool>("feat_autosave_interval",false);
    eng->autosaveIntervalSec=mod->getSavedValue<double>("feat_autosave_interval_sec", 180.0);
    eng->replayBackupsEnabled=mod->getSavedValue<bool>("feat_replay_backups",true);
    eng->updater.m_ssbFix=mod->getSavedValue<bool>("feat_scroll_speed_fix",false);
    eng->updater.m_lockDelta=mod->getSavedValue<bool>("feat_lock_delta",true);
    eng->updater.m_extrapolateFrames=mod->getSavedValue<bool>("feat_frame_extrapolation",false);
    eng->hud.enabled=mod->getSavedValue<bool>("hud_enabled",false);
    eng->hud.showFrame=mod->getSavedValue<bool>("hud_show_frame",true);
    eng->hud.showTPS=mod->getSavedValue<bool>("hud_show_tps",false);
    eng->hud.showX=mod->getSavedValue<bool>("hud_show_x",false);
    eng->hud.showY=mod->getSavedValue<bool>("hud_show_y",false);
    eng->hud.showXVel=mod->getSavedValue<bool>("hud_show_xvel",false);
    eng->hud.showYVel=mod->getSavedValue<bool>("hud_show_yvel",false);
    eng->hud.showRot=mod->getSavedValue<bool>("hud_show_rot",false);
    eng->hud.showState=mod->getSavedValue<bool>("hud_show_state",false);
    eng->hud.anchor=mod->getSavedValue<int>("hud_anchor",0);
    eng->hud.bigFont=mod->getSavedValue<bool>("hud_big_font",false);
    eng->hud.opacity=mod->getSavedValue<float>("hud_opacity",1.f);
    eng->hud.scale=mod->getSavedValue<float>("hud_scale",0.7f);
    megaHackLook=mod->getSavedValue<bool>("ui_megahack_look",false);
    compactMode=mod->getSavedValue<bool>("ui_compact_mode",false);
        {
        std::string enc=mod->getSavedValue<std::string>("fw_tiers","");
        eng->fwTiers.clear();
        size_t pos=0;
        while(pos<enc.size()){
            size_t semi=enc.find(';',pos);
            if(semi==std::string::npos)break;
            std::string row=enc.substr(pos,semi-pos);
            pos=semi+1;
                        std::vector<std::string> f; size_t fp=0;
            while(fp<=row.size()){
                size_t bar=row.find('|',fp);
                if(bar==std::string::npos){f.push_back(row.substr(fp));break;}
                f.push_back(row.substr(fp,bar-fp)); fp=bar+1;
            }
            if(f.size()>=7){
                GucciEngine::FrameWindowTier t;
                t.lo=atoi(f[0].c_str()); t.hi=atoi(f[1].c_str());
                snprintf(t.imageFile,sizeof(t.imageFile),"%s",f[2].c_str());
                snprintf(t.soundFile,sizeof(t.soundFile),"%s",f[3].c_str());
                t.r=(float)atof(f[4].c_str()); t.g=(float)atof(f[5].c_str()); t.b=(float)atof(f[6].c_str());
                eng->fwTiers.push_back(t);
            }
        }
    }
    theme.applyToImGuiStyle();}

void MenuInterface::drawInterface(){
    auto* engine=GucciEngine::get();
    if(!setupComplete)return;
        // Freshly computed every frame, not a sticky navigation flag: the old
    // jupiterClickBarPageOpen (still used for navigation -- which page to
    // render) only gets cleared by its own Back button, so leaving the
    // Click Trainer page via any other route (menu-close hotkey, switching
    // tabs) left it stuck true, and keybinds.cpp's click-tracking hook --
    // which only checks that flag -- kept feeding ordinary gameplay jumps
    // into the click bar's history indefinitely. This is reset to false
    // here unconditionally every frame and only set true inside
    // drawJupiterClickTrainerPage when it actually renders that frame, so
    // it can never go stale.
    engine->jupiterClickBarPageVisible=false;
    engine->trainerClickBarPageVisible=false;
    anim.update(ImGui::GetIO().DeltaTime);
        if(!anim.closing&&!anim.opening&&anim.openProgress<=0.f&&shown){
        shown=false;
        previouslyShown=false;
    }
    if(shown&&!previouslyShown&&engine){engine->reloadMacroList();previouslyShown=true;}
    if(shown&&!anim.closing)PlatformToolbox::showCursor();
    theme.applyToImGuiStyle();
    drawBackdrop();
    if(anim.openProgress>0.f){
        if(compactMode)drawCompactWindow();
        else if(megaHackLook)drawMegaHackWindow();
        else drawMainWindow();
    }
    drawRenderCompletePopup();}

void MenuInterface::drawRenderCompletePopup(){
    auto* engine=GucciEngine::get();
    auto& lr=engine->renderer.lastRender;
    if(lr.pending){lr.pending=false;ImGui::OpenPopup("RenderComplete");}
    ImGui::SetNextWindowSize(ImVec2(380,0),ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16,14));
    if(ImGui::BeginPopupModal("RenderComplete",nullptr,
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize)){
        ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
        if(fontHeading)ImGui::PushFont(fontHeading);
        ImGui::TextUnformatted(lr.success?"Render Complete":"Render Failed");
        if(fontHeading)ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0,8));
        if(lr.success){
            std::string fname=std::filesystem::path(lr.path).filename().string();
            double mb=(double)lr.fileSize/(1024.0*1024.0);
            ImGui::Text("File: %s",fname.c_str());
            ImGui::Text("Resolution: %ux%u @ %ufps",lr.width,lr.height,lr.fps);
            int ds=(int)lr.duration;
            ImGui::Text("Duration: %d:%02d",ds/60,ds%60);
            if(mb>=0.01)ImGui::Text("Size: %.2f MB",mb);
            else ImGui::Text("Size: %llu bytes",(unsigned long long)lr.fileSize);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,theme.textSecondary);
            ImGui::TextWrapped("The render did not finish successfully. Check the log for details.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0,12));
        float bw=(ImGui::GetContentRegionAvail().x-8)/2.f;
        if(Widgets::StyledButton("Open Folder",ImVec2(bw,30),theme,anim,6.f)){
            std::error_code ec;
            auto folder=std::filesystem::path(lr.path).parent_path();
            if(std::filesystem::exists(folder,ec))utils::file::openFolder(folder);
        }
        ImGui::SameLine(0,8);
        if(Widgets::StyledButton("Close",ImVec2(bw,30),theme,anim,6.f))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();}

void MenuInterface::initialize(){
    auto& io=ImGui::GetIO();
    io.FontGlobalScale=theme.textScale;
    auto* mod=Mod::get();
    auto fontPath=mod->getResourcesDir()/"Roboto-Regular.ttf";
    auto boldPath=mod->getResourcesDir()/"Roboto-Bold.ttf";
    if(std::filesystem::exists(fontPath)){
        fontBody=io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(),14.f);
        fontSmall=io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(),11.f);
        if(std::filesystem::exists(boldPath)){
            fontHeading=io.Fonts->AddFontFromFileTTF(boldPath.string().c_str(),16.f);
            fontTitle=io.Fonts->AddFontFromFileTTF(boldPath.string().c_str(),28.f);
        }
    }
    loadSettings();
    theme.applyToImGuiStyle();
    setupComplete=true;
}

void displayOverlayBranding(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete)return;
        if(!ui->shown)return;
    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x-10,vp->Pos.y+vp->Size.y-10),ImGuiCond_Always,ImVec2(1,1));
    ImGui::SetNextWindowSize(ImVec2(0,0),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin("##wm",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(ui->fontSmall)ImGui::PushFont(ui->fontSmall);
    ImVec4 a=ui->theme.getAccent();
    const char* brand=(ui->activeTheme==THEME_TOOSII)?
        "ToosiiBot v" MOD_VERSION "  Open!":
        "GucciBot v" MOD_VERSION "  Brrr.";
    ImGui::TextColored(ImVec4(a.x,a.y,a.z,0.55f),"%s",brand);
    if(ui->fontSmall)ImGui::PopFont();
    ImGui::End();}

void displayRenderHUD(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete||!engine)return;
    auto& r=engine->renderer;
    if(!r.recording)return;
    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x-10,vp->Pos.y+10),ImGuiCond_Always,ImVec2(1,0));
    ImGui::SetNextWindowSize(ImVec2(0,0),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##renderhud",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(ui->fontBody)ImGui::PushFont(ui->fontBody);
    float pulse=0.55f+0.45f*std::sin((float)ImGui::GetTime()*4.f);
    ImGui::TextColored(ImVec4(1.f,0.25f,0.25f,pulse),"REC");
    ImGui::SameLine();
    int frames=(int)r.renderedFrames.size();
    int secs=(int)r.lastFrame_t;
    ImGui::Text("%d frames  %d:%02d  @%ufps",frames,secs/60,secs%60,r.fps);
    if(ui->fontBody)ImGui::PopFont();
    ImGui::End();}

void displayCalculatingHUD(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete||!engine)return;
    // Nigel: no way to tell Calculate is running, or to stop it, without the
    // menu open on the Frame Windows tab specifically -- unlike drawInterface()'s
    // windows, this one is deliberately NOT gated on ui->shown/anim.openProgress,
    // same as the other display*HUD overlays below.
    if(!engine->fwAnalyzing)return;

    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+vp->Size.x-10,vp->Pos.y+10),ImGuiCond_Always,ImVec2(1,0));
    ImGui::SetNextWindowSize(ImVec2(0,0),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##calcHud",nullptr,ImGuiWindowFlags_NoDecoration|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(ui->fontBody)ImGui::PushFont(ui->fontBody);
    float pulse=0.55f+0.45f*std::sin((float)ImGui::GetTime()*4.f);
    ImVec4 accent=ui->theme.getAccent();
    ImGui::TextColored(ImVec4(accent.x,accent.y,accent.z,pulse),"Calculating...");
    if(ui->fontBody)ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text,ui->theme.textSecondary);
    if(engine->fwAnalyzeTotal>0)
        ImGui::Text("%s  %d/%d  (%.0f%%)",engine->fwAnalyzeStage.c_str(),
            engine->fwAnalyzeCur,engine->fwAnalyzeTotal,engine->fwAnalyzeProgress*100.f);
    else
        ImGui::Text("%s  (%.0f%%)",engine->fwAnalyzeStage.c_str(),engine->fwAnalyzeProgress*100.f);
    ImGui::PopStyleColor();
    if(Widgets::StyledButton("Cancel",ImVec2(-1,24),ui->theme,ui->anim,4.f))engine->cancelAnalysis();
    ImGui::End();
}

void displayFwLegendHUD(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete||!engine)return;
    if(!engine->fwLegendEnabled||!engine->fwHasData||engine->fwTiers.empty())return;
    if(!PlayLayer::get())return;

    uint32_t curFrame=engine->updater.getFrame();

    // Tally marks reached so far (frame <= curFrame) into whichever Tier
    // each one's window falls into -- mirrors the marker overlay's own
    // progressive reveal (framewindow.cpp's render()), so the counter fills
    // in exactly in step with the markers appearing on screen, same as the
    // reference frame-window-counter overlays this was modeled on.
    std::vector<int> counts(engine->fwTiers.size(),0);
    for(auto const& mk:engine->fwMarks){
        if(mk.frame>curFrame)continue;
        for(size_t i=0;i<engine->fwTiers.size();++i){
            auto const& t=engine->fwTiers[i];
            if(mk.window>=t.lo&&mk.window<=t.hi){counts[i]++;break;}
        }
    }

    // Loosest tier first (top), tightest last (bottom) -- matches the
    // reference layout regardless of what order the tiers are actually
    // configured/stored in.
    std::vector<size_t> order(engine->fwTiers.size());
    for(size_t i=0;i<order.size();++i)order[i]=i;
    std::sort(order.begin(),order.end(),[&](size_t a,size_t b){
        return engine->fwTiers[a].hi>engine->fwTiers[b].hi;
    });

    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+10,vp->Pos.y+10),ImGuiCond_Always,ImVec2(0,0));
    ImGui::SetNextWindowSize(ImVec2(0,0),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fwLegend",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(ui->fontBody)ImGui::PushFont(ui->fontBody);
    for(size_t idx:order){
        auto const& t=engine->fwTiers[idx];
        ImVec4 col(t.r,t.g,t.b,1.f);
        if(t.lo==t.hi) ImGui::TextColored(col,"%d: %d",t.lo,counts[idx]);
        else           ImGui::TextColored(col,"%d-%d: %d",t.lo,t.hi,counts[idx]);
    }
    if(ui->fontBody)ImGui::PopFont();
    ImGui::End();
}

void displayGameplayHUD(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete||!engine)return;
    auto& h=engine->hud;
    if(!h.enabled)return;
    auto* pl=PlayLayer::get();
    if(!pl||!pl->m_player1)return;
    auto* p=pl->m_player1;

        char buf[512]; buf[0]='\0'; int n=0;
    auto add=[&](const char* fmt,auto val){
        char line[96]; snprintf(line,sizeof(line),fmt,val);
        if(n++)strncat(buf,"\n",sizeof(buf)-strlen(buf)-1);
        strncat(buf,line,sizeof(buf)-strlen(buf)-1);
    };
    if(h.showFrame) add("Frame: %u",engine->updater.getFrame());
    if(h.showTPS)   add("TPS: %.0f",engine->updater.m_tps);
    if(h.showX)     add("X: %.1f",p->m_position.x);
    if(h.showY)     add("Y: %.1f",p->m_position.y);
    if(h.showXVel)  add("X Vel: %.2f",p->getCurrentXVelocity());
    if(h.showYVel)  add("Y Vel: %.2f",p->m_yVelocity);
    if(h.showRot)   add("Rot: %.0f",p->getRotation());
    if(h.showState) add("On ground: %s",p->m_isOnGround?"yes":"no");
    if(n==0)return;

    auto* vp=ImGui::GetMainViewport();
        float px = (h.anchor==1||h.anchor==3) ? vp->Pos.x+vp->Size.x-10 : vp->Pos.x+10;
    float py = (h.anchor==2||h.anchor==3) ? vp->Pos.y+vp->Size.y-10 : vp->Pos.y+10;
    ImVec2 pivot((h.anchor==1||h.anchor==3)?1.f:0.f,(h.anchor==2||h.anchor==3)?1.f:0.f);
    ImGui::SetNextWindowPos(ImVec2(px,py),ImGuiCond_Always,pivot);
    ImGui::SetNextWindowBgAlpha(0.45f*h.opacity);
    ImGui::Begin("##gbhud",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(h.bigFont&&ui->fontHeading)ImGui::PushFont(ui->fontHeading);
    else if(ui->fontBody)ImGui::PushFont(ui->fontBody);
    ImGui::SetWindowFontScale(h.scale);
    ImGui::TextColored(ImVec4(1,1,1,h.opacity),"%s",buf);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::End();
}

void displayAccuracyHUD(){
    auto* ui=MenuInterface::get();
    auto* engine=GucciEngine::get();
    if(!ui||!ui->setupComplete||!engine)return;
    if(!engine->accuracyHudEnabled)return;
    if(!PlayLayer::get())return;

    int pct=engine->accuracyTotalClicks>0
        ? (int)((float)engine->accuracyGoodClicks/(float)engine->accuracyTotalClicks*100.f+0.5f)
        : 100;

    auto* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+10,vp->Pos.y+vp->Size.y-10),ImGuiCond_Always,ImVec2(0,1));
    ImGui::SetNextWindowSize(ImVec2(0,0),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##accuracyhud",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    if(ui->fontBody)ImGui::PushFont(ui->fontBody);
    ImGui::Text("Accuracy: %d%%   Streak: %d (Best: %d)",pct,engine->currentStreak,engine->bestStreak);
    if(ui->fontBody)ImGui::PopFont();
    ImGui::End();
}

$on_mod(Loaded){
    ImGuiCocos::get()
        .setup([]{MenuInterface::get()->initialize();})
        .draw([]{
            auto* ui=MenuInterface::get();
            ui->drawInterface();
            displayOverlayBranding();
            displayRenderHUD();
            displayCalculatingHUD();
            displayFwLegendHUD();
            displayGameplayHUD();
            displayAccuracyHUD();});}
