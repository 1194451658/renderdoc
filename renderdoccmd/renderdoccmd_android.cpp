/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2017 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "renderdoccmd.h"
#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <string>

#include <android_native_app_glue.h>
#define ANativeActivity_onCreate __attribute__((visibility("default"))) ANativeActivity_onCreate
extern "C" {
#include <android_native_app_glue.c>
}

#include <android/log.h>
#define ANDROID_LOG(...) __android_log_print(ANDROID_LOG_INFO, "renderdoc", __VA_ARGS__);

using std::string;
using std::vector;
using std::istringstream;

struct android_app *android_state;
pthread_t cmdthread_handle = 0;

struct
{
  pthread_mutex_t lock;
  pthread_mutexattr_t attr;
} m_DrawLock;

void Daemonise()
{
}

void DisplayRendererPreview(IReplayController *renderer, TextureDisplay &displayCfg, uint32_t width,
                            uint32_t height)
{
  ANativeWindow *connectionScreenWindow = android_state->window;

  pthread_mutex_lock(&m_DrawLock.lock);

  IReplayOutput *out = renderer->CreateOutput(CreateAndroidWindowingData(connectionScreenWindow),
                                              ReplayOutputType::Texture);

  out->SetTextureDisplay(displayCfg);

  for(int i = 0; i < 100; i++)
  {
    renderer->SetFrameEvent(10000000, true);

    ANDROID_LOG("Frame %i", i);
    out->Display();

    usleep(100000);
  }

  pthread_mutex_unlock(&m_DrawLock.lock);
}

void DisplayGenericSplash()
{
  // if something else is drawing and holding the lock, then bail
  if(pthread_mutex_trylock(&m_DrawLock.lock))
    return;

  // since we're not pumping this continually and we only draw when we need to, we can just do the
  // full initialisation and teardown every time. This means we don't have to pay attention to
  // whether something else needs to create a context on the window - we just do it
  // opportunistically when we can hold the draw lock.
  // This only takes about 30ms anyway, so it's still technically realtime, right!?

  // fetch everything dynamically. We can't link against libEGL otherwise it messes with the hooking
  // in the main library. Just declare local function pointers with the real names
  void *libEGL = dlopen("libEGL.so", RTLD_NOW);

#define DLSYM_GET(name) decltype(&::name) name = (decltype(&::name))dlsym(libEGL, #name);
#define GPA_GET(name) decltype(&::name) name = (decltype(&::name))eglGetProcAddress(#name);

  DLSYM_GET(eglBindAPI);
  DLSYM_GET(eglGetDisplay);
  DLSYM_GET(eglInitialize);
  DLSYM_GET(eglChooseConfig);
  DLSYM_GET(eglCreateContext);
  DLSYM_GET(eglCreateWindowSurface);
  DLSYM_GET(eglMakeCurrent);
  DLSYM_GET(eglDestroySurface);
  DLSYM_GET(eglDestroyContext);
  DLSYM_GET(eglGetProcAddress);
  DLSYM_GET(eglSwapBuffers);

  GPA_GET(glCreateShader);
  GPA_GET(glShaderSource);
  GPA_GET(glCompileShader);
  GPA_GET(glCreateProgram);
  GPA_GET(glAttachShader);
  GPA_GET(glLinkProgram);
  GPA_GET(glGetUniformLocation);
  GPA_GET(glUniform2f);
  GPA_GET(glUseProgram);
  GPA_GET(glVertexAttribPointer);
  GPA_GET(glEnableVertexAttribArray);
  GPA_GET(glDrawArrays);

  eglBindAPI(EGL_OPENGL_ES_API);

  EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  ANativeWindow *previewWindow = android_state->window;

  if(eglDisplay)
  {
    int major = 0, minor = 0;
    eglInitialize(eglDisplay, &major, &minor);

    const EGLint configAttribs[] = {EGL_RED_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_SURFACE_TYPE,
                                    EGL_WINDOW_BIT,
                                    EGL_COLOR_BUFFER_TYPE,
                                    EGL_RGB_BUFFER,
                                    EGL_NONE};

    EGLint numConfigs;
    EGLConfig config;
    if(eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs))
    {
      // we only need GLES 2 for this
      static const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

      EGLContext ctx = eglCreateContext(eglDisplay, config, NULL, ctxAttribs);
      if(ctx)
      {
        EGLSurface surface = eglCreateWindowSurface(eglDisplay, config, previewWindow, NULL);

        if(surface)
        {
          eglMakeCurrent(eglDisplay, surface, surface, ctx);

          // simple pass through shader for the fullscreen triangle verts
          const char *vertex =
              "attribute vec2 pos;\n"
              "void main() { gl_Position = vec4(pos, 0.5, 1.0); }";

          const char *fragment = R"(
float circle(in vec2 uv, in vec2 centre, in float radius)
{
  return length(uv - centre) - radius;
}

// distance field for RenderDoc logo
float logo(in vec2 uv)
{
  // add the outer ring
  float ret = circle(uv, vec2(0.5, 0.5), 0.275);

  // add the upper arc
  ret = min(ret, circle(uv, vec2(0.5, -0.37), 0.71));

  // subtract the inner ring
  ret = max(ret, -circle(uv, vec2(0.5, 0.5), 0.16));

  // subtract the lower arc
  ret = max(ret, -circle(uv, vec2(0.5, -1.085), 1.3));

  return ret;
}

uniform vec2 iResolution;

void main()
{
  vec2 uv = gl_FragCoord.xy / iResolution.xy;

  // centre the UVs in a square. This assumes a landscape layout.
  uv.x = 0.5 - (uv.x - 0.5) * (iResolution.x / iResolution.y);

  // this constant here can be tuned depending on DPI to increase AA
  float edgeWidth = 10.0/max(iResolution.x, iResolution.y);

  float smoothdist = smoothstep(0.0, edgeWidth, clamp(logo(uv), 0.0, 1.0));

  // the green is #3bb779
  gl_FragColor = mix(vec4(1.0), vec4(0.2314, 0.7176, 0.4745, 1.0), smoothdist);
}
)";

          // compile the shaders and link into a program
          GLuint vs = glCreateShader(GL_VERTEX_SHADER);
          glShaderSource(vs, 1, &vertex, NULL);
          glCompileShader(vs);
          GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
          glShaderSource(fs, 1, &fragment, NULL);
          glCompileShader(fs);
          GLuint prog = glCreateProgram();
          glAttachShader(prog, vs);
          glAttachShader(prog, fs);
          glLinkProgram(prog);
          glUseProgram(prog);

          // set the resolution
          GLuint loc = glGetUniformLocation(prog, "iResolution");
          glUniform2f(loc, float(ANativeWindow_getWidth(previewWindow)),
                      float(ANativeWindow_getHeight(previewWindow)));

          // fullscreen triangle
          float verts[] = {
              -1.0f, -1.0f,    // vertex 0
              3.0f,  -1.0f,    // vertex 1
              -1.0f, 3.0f,     // vertex 2
          };

          glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, verts);
          glEnableVertexAttribArray(0);

          glDrawArrays(GL_TRIANGLES, 0, 3);

          eglSwapBuffers(eglDisplay, surface);
        }
        else
        {
          ANDROID_LOG("failed making surface");
        }

        eglMakeCurrent(eglDisplay, 0L, 0L, NULL);
        eglDestroyContext(eglDisplay, ctx);
        eglDestroySurface(eglDisplay, surface);
      }
      else
      {
        ANDROID_LOG("failed making context");
      }
    }
    else
    {
      ANDROID_LOG("failed choosing config");
    }
  }

  dlclose(libEGL);

  pthread_mutex_unlock(&m_DrawLock.lock);
}

// Returns the renderdoccmd arguments passed via am start
// Examples: am start ... -e renderdoccmd "remoteserver"
// -e renderdoccmd "replay /sdcard/capture.rdc"
vector<string> getRenderdoccmdArgs()
{
  JNIEnv *env;
  android_state->activity->vm->AttachCurrentThread(&env, 0);

  jobject me = android_state->activity->clazz;

  jclass acl = env->GetObjectClass(me);    // class pointer of NativeActivity
  jmethodID giid = env->GetMethodID(acl, "getIntent", "()Landroid/content/Intent;");
  jobject intent = env->CallObjectMethod(me, giid);    // Got our intent

  jclass icl = env->GetObjectClass(intent);    // class pointer of Intent
  jmethodID gseid =
      env->GetMethodID(icl, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");

  jstring jsParam1 =
      (jstring)env->CallObjectMethod(intent, gseid, env->NewStringUTF("renderdoccmd"));

  vector<string> ret;
  if(!jsParam1)
    return ret;    // No arg value found

  ret.push_back("renderdoccmd");

  const char *param1 = env->GetStringUTFChars(jsParam1, 0);
  istringstream iss(param1);
  while(iss)
  {
    string sub;
    iss >> sub;
    ret.push_back(sub);
  }
  android_state->activity->vm->DetachCurrentThread();

  return ret;
}

void *cmdthread(void *)
{
  vector<string> args = getRenderdoccmdArgs();
  if(!args.size())
    return NULL;    // Nothing for APK to do.

  renderdoccmd(GlobalEnvironment(), args);

  // activity is done and should be closed
  ANativeActivity_finish(android_state->activity);

  return NULL;
}

void handle_cmd(android_app *app, int32_t cmd)
{
  switch(cmd)
  {
    case APP_CMD_INIT_WINDOW:
    {
      if(cmdthread_handle == 0)
        pthread_create(&cmdthread_handle, NULL, cmdthread, NULL);

      break;
    }
    case APP_CMD_WINDOW_REDRAW_NEEDED:
    case APP_CMD_GAINED_FOCUS:
    case APP_CMD_LOST_FOCUS:
    {
      DisplayGenericSplash();
      break;
    }
  }
}

void android_main(struct android_app *state)
{
  android_state = state;
  android_state->onAppCmd = handle_cmd;

  pthread_mutexattr_init(&m_DrawLock.attr);
  pthread_mutexattr_settype(&m_DrawLock.attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m_DrawLock.lock, &m_DrawLock.attr);

  ANDROID_LOG("android_main android_state->window: %p", android_state->window);

  // Used to poll the events in the main loop
  int events;
  android_poll_source *source;
  do
  {
    if(ALooper_pollAll(1, nullptr, &events, (void **)&source) >= 0)
    {
      if(source != NULL)
        source->process(android_state, source);
    }
  } while(android_state->destroyRequested == 0);

  ANDROID_LOG("android_main exiting");

  pthread_mutex_destroy(&m_DrawLock.lock);
  pthread_mutexattr_destroy(&m_DrawLock.attr);
}
