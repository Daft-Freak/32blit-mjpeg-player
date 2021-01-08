#include <cstring>

#include "mjpeg-player.hpp"

#include "assets.hpp"
#include "file-browser.hpp"

#include "avi-file.hpp"

#ifdef PROFILER
#include "engine/profiler.hpp"

blit::Profiler profiler;
blit::ProfilerProbe *profilerUpdateProbe;
blit::ProfilerProbe *profilerRenderProbe;
blit::ProfilerProbe *profilerVidReadProbe;
blit::ProfilerProbe *profilerVidDecProbe;
blit::ProfilerProbe *profilerAudReadProbe;
#endif

/*
mp3
ffmpeg -i 2020-02-19\ 12-14-08.mkv -vcodec mjpeg -q:v 2 -pix_fmt yuvj420p -vf scale=w=320:h=240:force_original_aspect_ratio=decrease,fps=fps=25 \
-acodec libmp3lame -ar 22050 -ac 1 test420_mp3.avi

raw

ffmpeg -i 2020-02-19\ 12-14-08.mkv -vcodec mjpeg -q:v 2 -pix_fmt yuvj420p -vf scale=w=320:h=240:force_original_aspect_ratio=decrease,fps=fps=25 \
-acodec pcm_s16le -ar 22050 -ac 1 test420_raw.avi

*/

const blit::Font tallFont(asset_tall_font);
FileBrowser fileBrowser(tallFont);
std::string fileToLoad;
bool renderedLoadMessage = false;

AVIFile avi;

void openFile(std::string filename)
{
   // delay loading so that we can show the loading message
   renderedLoadMessage = false;
   fileToLoad = filename;
}

void init()
{
    blit::set_screen_mode(blit::ScreenMode::hires);

#ifdef PROFILER
    profiler.SetDisplaySize(blit::screen.bounds.w, blit::screen.bounds.h);
    profiler.SetRows(5);
    profiler.SetAlpha(200);
    profiler.DisplayHistory(true);

    profiler.SetupGraphElement(blit::Profiler::dmCur, true, true, blit::Pen(0, 255, 0));
    profiler.SetupGraphElement(blit::Profiler::dmAvg, true, true, blit::Pen(0, 255, 255));
    profiler.SetupGraphElement(blit::Profiler::dmMax, true, true, blit::Pen(255, 0, 0));
    profiler.SetupGraphElement(blit::Profiler::dmMin, true, true, blit::Pen(255, 255, 0));

    profilerRenderProbe = profiler.AddProbe("Render", 300);
    profilerUpdateProbe = profiler.AddProbe("Update", 300);
    profilerVidReadProbe = profiler.AddProbe("JPEG Read", 300);
    profilerVidDecProbe = profiler.AddProbe("JPEG Decode", 300);
    profilerAudReadProbe = profiler.AddProbe("Audio Read", 300);
#endif

    fileBrowser.set_extensions({".avi"});
    fileBrowser.set_display_rect(blit::Rect(0, 0, blit::screen.bounds.w, blit::screen.bounds.h));
    fileBrowser.set_on_file_open(openFile);
    fileBrowser.init();

    auto launchPath = blit::get_launch_path();
    if(launchPath)
    {
        std::string pathStr(launchPath);
        auto pos = pathStr.find_last_of('/');
        if(pos != std::string::npos)
            fileBrowser.set_current_dir(pathStr.substr(0, pos));

        openFile(launchPath);
    }
}

void render(uint32_t time_ms)
{
#ifdef PROFILER
    profilerRenderProbe->Start();
#endif

    blit::screen.alpha = 0xFF;
    blit::screen.pen = blit::Pen(20, 30, 40);
    blit::screen.clear();

    if(!fileToLoad.empty())
    {
        blit::screen.pen = blit::Pen(0xFF, 0xFF, 0xFF);
        blit::screen.text("Please wait...", blit::minimal_font, blit::Point(blit::screen.bounds.w / 2, blit::screen.bounds.h / 2), true, blit::TextAlign::center_center);
        renderedLoadMessage = true;
        return;
    }

    if(avi.getPlaying())
        avi.render();
    else
        fileBrowser.render();

#ifdef PROFILER
    profilerRenderProbe->StoreElapsedUs();

    profiler.DisplayProbeOverlay(1);
#endif
}

void update(uint32_t time_ms)
{
#ifdef PROFILER
    profiler.SetGraphTime(profilerUpdateProbe->ElapsedMetrics().uMaxElapsedUs);
    blit::ScopedProfilerProbe scopedProbe(profilerUpdateProbe);
#endif

    static uint32_t lastButtonState = 0;

    if(avi.getPlaying())
    {
        // b released
        if((lastButtonState & blit::Button::B) && !(blit::buttons & blit::Button::B))
            avi.stop();

        avi.update(time_ms);
    }
    else
        fileBrowser.update(time_ms);

    // load file
    if(!fileToLoad.empty() && renderedLoadMessage)
    {
        if(avi.load(fileToLoad))
            avi.play(0);
        fileToLoad = "";
    }

    lastButtonState = blit::buttons;
}
