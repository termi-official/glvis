// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443271. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the GLVis visualization tool and library. For more
// information and source code availability see http://glvis.org.
//
// GLVis is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include <iostream>
#include <chrono>
#include "sdl.hpp"
#include <SDL2/SDL_syswm.h>
#include "visual.hpp"
#include "gl/renderer_core.hpp"
#include "gl/renderer_ff.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using std::cerr;
using std::endl;

#ifdef GLVIS_DEBUG
#define PRINT_DEBUG(s) std::cerr << s
#else
#define PRINT_DEBUG(s) {}
#endif

extern int GetMultisample();
extern int visualize;

struct SdlWindow::_SdlHandle
{
    SDL_Window * hwnd;
    SDL_GLContext gl_ctx;
    _SdlHandle(const std::string& title, int x, int y, int w, int h, Uint32 wndflags)
        : hwnd(nullptr)
        , gl_ctx(0) {
        hwnd = SDL_CreateWindow(title.c_str(), x, y, w, h, wndflags);
        if (!hwnd) {
            PRINT_DEBUG("SDL window creation failed with error: " << SDL_GetError() << endl);
            return;
        }
        gl_ctx = SDL_GL_CreateContext(hwnd);
        if (!gl_ctx) {
            PRINT_DEBUG("OpenGL context creation failed with error: " << SDL_GetError() << endl);
        }
    }

    ~_SdlHandle() {
        if (gl_ctx)
            SDL_GL_DeleteContext(gl_ctx);
        if (hwnd)
            SDL_DestroyWindow(hwnd);
    }

    bool isInitialized() {
        return (hwnd != nullptr && gl_ctx != 0);
    }
};

bool SdlWindow::isGlInitialized() {
    return (_handle->gl_ctx != 0);
}

SdlWindow::SdlWindow()
    : onIdle(nullptr)
    , onExpose(nullptr)
    , onReshape(nullptr)
    , ctrlDown(false)
    , requiresExpose(false)
    , takeScreenshot(false) {
}

bool SdlWindow::createWindow(const char * title, int x, int y, int w, int h) {
    if (!SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            cerr << "FATAL: Failed to initialize SDL: " << SDL_GetError() << endl;
            return false;
        }
    }

    //destroy any existing SDL window
    _handle.reset();

    // If we want to use WebGL 2:
    // #ifdef __EMSCRIPTEN__
    // SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    // SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    // SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    // #endif

    Uint32 win_flags = SDL_WINDOW_OPENGL;
#ifndef __EMSCRIPTEN__
    win_flags |= SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
#endif
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24);
    if (GetMultisample() > 0) {
        SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, GetMultisample());
    }
#ifndef __EMSCRIPTEN__
    PRINT_DEBUG("Creating window with OpenGL core profile..." << flush);
    // Try to create core profile context first
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    _handle.reset(new _SdlHandle(title, x, y, w, h, win_flags));

    if (!_handle->isInitialized()) {
        PRINT_DEBUG("failed." << endl << "Falling back to the default profile..." << flush);
        // Unset core profile flag, which should give us a legacy OpenGL context
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        _handle.reset(new _SdlHandle(title, x, y, w, h, win_flags));
    }

    if (!_handle->isInitialized()) {
        PRINT_DEBUG("failed." << endl << "Falling back to legacy OpenGL..." << flush);
        // Unset core profile flag, which should give us a legacy OpenGL context
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        _handle.reset(new _SdlHandle(title, x, y, w, h, win_flags));
    }
#else
    cerr << "Creating window..." << flush;
    _handle.reset(new _SdlHandle(title, x, y, w, h, win_flags));
#endif
    // at this point, window should be up
    if (!_handle->isInitialized()) {
        PRINT_DEBUG(cerr << "failed." << endl);
        cerr << "FATAL: window and/or OpenGL context creation failed." << endl;
        return false;
    } else {
        PRINT_DEBUG("done." << endl);
    }

#ifndef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(0);
    glEnable(GL_DEBUG_OUTPUT);
#endif

    GLenum err = glewInit();
    if (err != GLEW_OK) {
        cerr << "FATAL: Failed to initialize GLEW: " << glewGetErrorString(err) << endl;
        return false;
    }

    // print verisons
    PRINT_DEBUG("Using GLEW " << glewGetString(GLEW_VERSION) << std::endl);
    PRINT_DEBUG("Using GL " << glGetString(GL_VERSION) << std::endl);

    SDL_version sdl_ver;
    SDL_GetVersion(&sdl_ver);
    PRINT_DEBUG("Using SDL " << (int)sdl_ver.major << "." << (int)sdl_ver.minor <<
      "." << (int)sdl_ver.patch << std::endl);

    _renderer.reset(new gl3::MeshRenderer);
#ifndef __EMSCRIPTEN__
	if (GLEW_EXT_transform_feedback) {
		glBindBufferBase            = glBindBufferBaseEXT;
		glTransformFeedbackVaryings = glTransformFeedbackVaryingsEXT;
		glBeginTransformFeedback    = glBeginTransformFeedbackEXT;
		glEndTransformFeedback      = glEndTransformFeedbackEXT;
	}
    if (GLEW_VERSION_3_0
        || (GLEW_VERSION_2_0 && GLEW_EXT_transform_feedback)) {
		// we require both shaders and transform feedback
		// EXT_transform_feedback was made core in OpenGL 3.0
        PRINT_DEBUG("Loading CoreGLDevice..." << endl);
        _renderer->setDevice<gl3::CoreGLDevice>();
    } else {
        PRINT_DEBUG("Shader support missing, loading FFGLDevice..." << endl);
        _renderer->setDevice<gl3::FFGLDevice>();
    }

#else
    _renderer->setDevice<gl3::CoreGLDevice>();
#endif

    return true;
}

// defined here because the _SdlHandle destructor needs to be visable
SdlWindow::~SdlWindow() {};

void SdlWindow::windowEvent(SDL_WindowEvent& ew) {
    switch(ew.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            if (onReshape)
                onReshape(ew.data1, ew.data2);
            break;
        case SDL_WINDOWEVENT_EXPOSED:
            if (onExpose)
                requiresExpose = true;
            break;
        default:
            break;
    }
}

void SdlWindow::motionEvent(SDL_MouseMotionEvent& em) {
    EventInfo info = {
        em.x, em.y,
        SDL_GetModState()
    };
    if (em.state & SDL_BUTTON_LMASK) {
        if (onMouseMove[SDL_BUTTON_LEFT]) {
            onMouseMove[SDL_BUTTON_LEFT](&info);
        }
    } else if (em.state & SDL_BUTTON_RMASK) {
        if (onMouseMove[SDL_BUTTON_RIGHT]) {
            onMouseMove[SDL_BUTTON_RIGHT](&info);
        }
    } else if (em.state & SDL_BUTTON_MMASK) {
        if (onMouseMove[SDL_BUTTON_MIDDLE]) {
            onMouseMove[SDL_BUTTON_MIDDLE](&info);
        }
    }
}

void SdlWindow::mouseEventDown(SDL_MouseButtonEvent& eb) {
    if (onMouseDown[eb.button]) {
        EventInfo info = {
            eb.x, eb.y,
            SDL_GetModState()
        };
        onMouseDown[eb.button](&info);
    }
}

void SdlWindow::mouseEventUp(SDL_MouseButtonEvent& eb) {
    if (onMouseUp[eb.button]) {
        EventInfo info = {
            eb.x, eb.y,
            SDL_GetModState()
        };
        onMouseUp[eb.button](&info);
    }
}

bool SdlWindow::keyEvent(SDL_Keysym& ks) {
    bool handled = false;
    if (ks.sym > 128 || ks.sym < 32) {
        if (onKeyDown[ks.sym])
            onKeyDown[ks.sym](ks.mod);
        handled = true;
    } else if (ctrlDown == true) {
        onKeyDown[ks.sym](ks.mod);
        handled = true;
    }
    if (ks.sym == SDLK_RCTRL || ks.sym == SDLK_LCTRL) {
        ctrlDown = true;
    }
    return handled;
}

bool SdlWindow::keyEvent(char c) {
    if (onKeyDown[c]) {
        onKeyDown[c](SDL_GetModState());
        return true;
    }
    return false;
}

bool SdlWindow::mainIter() {
    SDL_Event e;
    static bool useIdle = false;
    bool needsSwap = false;
    while (SDL_PollEvent(&e)) {
        bool renderKeyEvent = false;
        switch(e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                windowEvent(e.window);
                break;
            case SDL_KEYDOWN:
                renderKeyEvent = keyEvent(e.key.keysym);
                break;
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_LCTRL
                    || e.key.keysym.sym == SDLK_RCTRL) {
                    ctrlDown = false;
                }
                break;
            case SDL_TEXTINPUT:
                renderKeyEvent = keyEvent(e.text.text[0]);
                break;
            case SDL_MOUSEMOTION:
                motionEvent(e.motion);
                break;
            case SDL_MOUSEBUTTONDOWN:
                mouseEventDown(e.button);
                break;
            case SDL_MOUSEBUTTONUP:
                mouseEventUp(e.button);
                break;
        }
        if (renderKeyEvent)
            break;
    }
#ifndef __EMSCRIPTEN__
    if (onIdle) {
        if (glvis_command == NULL || visualize == 2 || useIdle) {
            onIdle();
            needsSwap = true;
        } else {
            if (glvis_command->Execute() < 0)
                running = false;
        }
        useIdle = !useIdle;
    }
    else {
      if (glvis_command->Execute() < 0) { running = false; }
    }
#else
    if (onIdle) {
        onIdle();
        needsSwap = true;
    }
#endif
    if (requiresExpose) {
        onExpose();
        requiresExpose = false;
        needsSwap = true;
    }
    return needsSwap;
}

void SdlWindow::mainLoop() {
    running = true;
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg([](void* arg) {
                                        ((SdlWindow*) arg)->mainIter();
                                    }, this, 0, 1);
#else
    visualize = 1;
    while (running) {
        bool glSwap = mainIter();
        if (glSwap) {
            SDL_GL_SwapWindow(_handle->hwnd);
        }
        if (takeScreenshot) {
            Screenshot(screenshot_file.c_str());
            takeScreenshot = false;
        }
        // sleep for n milliseconds to avoid pegging CPU at 100%
        SDL_WaitEventTimeout(NULL, 10);
    }
#endif
}

void SdlWindow::getWindowSize(int& w, int& h) {
    if (_handle) {
#ifdef __EMSCRIPTEN__
        int is_fullscreen;
        emscripten_get_canvas_size(&w, &h, &is_fullscreen);
       // TODO: ^ is deprecated but we need to store the id somewhere
       /*
       EMSCRIPTEN_RESULT r = emscripten_get_canvas_element_size("#canvas", &w, &h);
       if (r != EMSCRIPTEN_RESULT_SUCCESS) {
         std::cerr << "emscripten error" << std::endl;
       }
       */
#else
        SDL_GL_GetDrawableSize(_handle->hwnd, &w, &h);
#endif
    }
}

const int default_dpi = 72;
void SdlWindow::getDpi(int& w, int& h) {
    if (_handle) {
        int disp = SDL_GetWindowDisplayIndex(_handle->hwnd);
        if (disp < 0) {
          PRINT_DEBUG("warning: problem getting display index: " << SDL_GetError() << endl);
          return;
        }

        float f_w, f_h;
        if (SDL_GetDisplayDPI(disp, NULL, &f_w, &f_h)) {
          PRINT_DEBUG("warning: problem getting dpi, setting to " << default_dpi << ": " <<
            SDL_GetError() << endl);
          w = default_dpi;
          h = default_dpi;
        }
        else {
          w = f_w;
          h = f_h;
        }
    }
    else {
      PRINT_DEBUG("warning: unable to get dpi: handle is null" << endl);
    }
}

#ifdef GLVIS_X11
Window SdlWindow::getXWindow() {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(window, &info)) {
        if (info.subsystem == SDL_SYSWM_X11) {
            return info.x11.window;
        }
    }
}
#endif

void SdlWindow::setWindowTitle(std::string& title) {
    setWindowTitle(title.c_str());
}

void SdlWindow::setWindowTitle(const char * title) {
    if (_handle) {
        SDL_SetWindowTitle(_handle->hwnd, title);
    }
}

void SdlWindow::setWindowSize(int w, int h) {
    if (_handle) {
        SDL_SetWindowSize(_handle->hwnd, w, h);
    }
}

void SdlWindow::setWindowPos(int x, int y) {
    if (_handle) {
        SDL_SetWindowPosition(_handle->hwnd, x, y);
    }
}

void SdlWindow::signalKeyDown(SDL_Keycode k, SDL_Keymod m) {
    SDL_Event event;
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = k;
    event.key.keysym.mod = m;
    SDL_PushEvent(&event);
}

void SdlWindow::swapBuffer() {
    SDL_GL_SwapWindow(_handle->hwnd);
}

