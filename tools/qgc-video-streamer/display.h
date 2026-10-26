// SDL2 local preview window — shows the highest available decoded frame.
// Crash-safe: if SDL fails, we just skip display without affecting upload.
#pragma once
#include <SDL2/SDL.h>
#include <cstdio>
#include "scaler.h"

class VideoDisplay {
    SDL_Window*   win  = nullptr;
    SDL_Renderer* rend = nullptr;
    SDL_Texture*  tex  = nullptr;
    int tw=0, th=0;
    bool ok=false;
public:
    VideoDisplay(const char* title, int w, int h) {
        if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS) != 0) {
            fprintf(stderr,"[display] SDL_Init: %s\n",SDL_GetError()); return;
        }
        win = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED, w,h,
            SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
        if (!win){ fprintf(stderr,"[display] window: %s\n",SDL_GetError()); return; }
        rend = SDL_CreateRenderer(win,-1,
            SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if (!rend) rend=SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE);
        if (!rend){ fprintf(stderr,"[display] renderer: %s\n",SDL_GetError()); return; }
        SDL_SetRenderDrawColor(rend,0,0,0,255);
        SDL_RenderClear(rend);
        SDL_RenderPresent(rend);
        ok = true;
    }
    ~VideoDisplay() {
        if(tex)  SDL_DestroyTexture(tex);
        if(rend) SDL_DestroyRenderer(rend);
        if(win)  SDL_DestroyWindow(win);
        if(ok)   SDL_Quit();
    }

    // Display a decoded I420 frame (planes from OpenH264 decoder).
    void present(uint8_t* yuv[3], int strides[2], int w, int h) {
        if (!ok) return;
        if (!tex || tw!=w || th!=h) {
            if(tex) SDL_DestroyTexture(tex);
            tex=SDL_CreateTexture(rend,SDL_PIXELFORMAT_IYUV,
                                   SDL_TEXTUREACCESS_STREAMING,w,h);
            tw=w; th=h;
        }
        if (!tex) return;
        SDL_UpdateYUVTexture(tex,nullptr,
            yuv[0],strides[0], yuv[1],strides[1], yuv[2],strides[1]);
        SDL_RenderClear(rend);
        SDL_RenderCopy(rend,tex,nullptr,nullptr);
        SDL_RenderPresent(rend);
    }

    // Display a scaled YUVFrame.
    void present_frame(const YUVFrame& f) {
        if (!ok) return;
        if (!tex || tw!=f.w || th!=f.h) {
            if(tex) SDL_DestroyTexture(tex);
            tex=SDL_CreateTexture(rend,SDL_PIXELFORMAT_IYUV,
                                   SDL_TEXTUREACCESS_STREAMING,f.w,f.h);
            tw=f.w; th=f.h;
        }
        if (!tex) return;
        SDL_UpdateYUVTexture(tex,nullptr,
            f.Y(),f.strideY(), f.U(),f.strideUV(), f.V(),f.strideUV());
        SDL_RenderClear(rend);
        SDL_RenderCopy(rend,tex,nullptr,nullptr);
        SDL_RenderPresent(rend);
    }

    // Returns false if window was closed.
    bool poll_events() {
        if (!ok) return true; // no window = keep running
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type==SDL_QUIT) return false;
            if (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) return false;
        }
        return true;
    }
    bool valid() const { return ok; }
};
