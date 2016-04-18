// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2015 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// To use this image viewer, first get image-magick development files
// $ sudo aptitude install libmagick++-dev
//
// Then compile with
// $ make led-image-viewer

#include "led-matrix.h"
#include "transformer.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <node.h>

#include <nan.h>

#include <vector>
#include <Magick++.h>
#include <magick/image.h>

//start
#include <node.h>
#include <v8.h>
#include <uv.h>
#include <string>
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>

using namespace v8;
//end

using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;
using rgb_matrix::GPIO;
using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::CanvasTransformer;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

bool showAnimation = false;

namespace {
// Preprocess as much as possible, so that we can just exchange full frames
// on VSync.
class PreprocessedFrame {
public:
  PreprocessedFrame(const Magick::Image &img,
                    CanvasTransformer *transformer,
                    rgb_matrix::FrameCanvas *output)
    : canvas_(output) {
    int delay_time = img.animationDelay();  // in 1/100s of a second.
    if (delay_time < 1) delay_time = 1;
    delay_micros_ = delay_time * 10000;

    Canvas *const transformed_draw_canvas = transformer->Transform(output);
    for (size_t y = 0; y < img.rows(); ++y) {
      for (size_t x = 0; x < img.columns(); ++x) {
        const Magick::Color &c = img.pixelColor(x, y);
        if (c.alphaQuantum() < 256) {
          transformed_draw_canvas
            ->SetPixel(x, y,
                       ScaleQuantumToChar(c.redQuantum()),
                       ScaleQuantumToChar(c.greenQuantum()),
                       ScaleQuantumToChar(c.blueQuantum()));
        }
      }
    }
  }

  FrameCanvas *canvas() const { return canvas_; }

  int delay_micros() const {
    return delay_micros_;
  }

  private:
    FrameCanvas *const canvas_;
    int delay_micros_;
  };
}  // end anonymous namespace

// Load still image or animation.
// Scale, so that it fits in "width" and "height" and store in "image_sequence".
// If this is a still image, "image_sequence" will contain one image, otherwise
// all animation frames.
static bool LoadAnimation(const char *filename, int width, int height,
                          std::vector<Magick::Image> *image_sequence) {
  std::vector<Magick::Image> frames;
  fprintf(stderr, "Read image...\n");
  readImages(&frames, filename);
  if (frames.size() == 0) {
    fprintf(stderr, "No image found.");
    return false;
  }

  // Put together the animation from single frames. GIFs can have nasty
  // disposal modes, but they are handled nicely by coalesceImages()
  if (frames.size() > 1) {
    fprintf(stderr, "Assembling animation with %d frames.\n",
            (int)frames.size());
    Magick::coalesceImages(image_sequence, frames.begin(), frames.end());
  } else {
    image_sequence->push_back(frames[0]);   // just a single still image.
  }

  fprintf(stderr, "Scale ... %dx%d -> %dx%d\n",
          (int)(*image_sequence)[0].columns(), (int)(*image_sequence)[0].rows(),
          width, height);
  for (size_t i = 0; i < image_sequence->size(); ++i) {
    (*image_sequence)[i].scale(Magick::Geometry(width, height));
  }
  return true;
}

// Preprocess buffers: create readily filled frame-buffers that can be
// swapped with the matrix to minimize computation time when we're displaying.
static void PrepareBuffers(const std::vector<Magick::Image> &images,
                           RGBMatrix *matrix,
                           std::vector<PreprocessedFrame*> *frames) {
  fprintf(stderr, "Preprocess for display.\n");
  CanvasTransformer *const transformer = matrix->transformer();
  for (size_t i = 0; i < images.size(); ++i) {
    FrameCanvas *canvas = matrix->CreateFrameCanvas();
    frames->push_back(new PreprocessedFrame(images[i], transformer, canvas));
  }
}

static void DisplayAnimation(const std::vector<PreprocessedFrame*> &frames,
                             RGBMatrix *matrix, bool play_once) {
  fprintf(stderr, "Display.\n");
  FrameCanvas *blankCanvas = matrix->CreateFrameCanvas();
  for (unsigned int i = 0; !interrupt_received; ++i) {
    if(showAnimation) {
      const PreprocessedFrame *frame = frames[i % frames.size()];
      matrix->SwapOnVSync(frame->canvas());
      if (frames.size() == 1 || (play_once == true && i == frames.size() - 1)) {
        sleep(86400);  // Only one image. Nothing to do.
      } else {
        usleep(frame->delay_micros());
      }
    } else {
      matrix->SwapOnVSync(blankCanvas);
    }
  }
}

struct Work {
  uv_work_t  request;
  std::string filename;
  Persistent<Function> callback;
};

RGBMatrix * matrix;
bool play_once = false;
std::vector<PreprocessedFrame*> frames;

static void MainAsync(uv_work_t *req) {
  /*v8::String::Utf8Value nameFromArgs(args[0]->ToString());
  std::string name = std::string(*nameFromArgs);*/
  Work *work = static_cast<Work *>(req->data);
  const char *filename = work->filename.c_str();
  
  Magick::InitializeMagick("");

  /*
   * Set up GPIO pins. This fails when not running as root.
   */
  GPIO io;
  io.Init();
  assert(io.Init());
  matrix = new RGBMatrix(&io, 32, 4, 1);
  matrix->SetBrightness(90);
  
  std::vector<Magick::Image> sequence_pics;
  if (!LoadAnimation(filename, matrix->width(), matrix->height(),
                     &sequence_pics)) {
    return;
  }

  PrepareBuffers(sequence_pics, matrix, &frames);
  
  DisplayAnimation(frames, matrix, play_once);
}

static void PlayGif(uv_work_t *req) {
  fprintf(stderr, "PlayGif\n");
  Work *work = static_cast<Work *>(req->data);
  showAnimation = true;
}

static void PlayGifComplete(uv_work_t *req, int status) {
  fprintf(stderr, "PlayGifComplete\n");
  Isolate * isolate = Isolate::GetCurrent();

  // Fix for Node 4.x - thanks to https://github.com/nwjs/blink/commit/ecda32d117aca108c44f38c8eb2cb2d0810dfdeb
  v8::HandleScope handleScope(isolate);
  Work *work = static_cast<Work *>(req->data);
  
  // set up return arguments
  const unsigned argc = 1;
  Local<Value> argv[argc] = { String::NewFromUtf8(isolate, "PLAYING GIF NOW") };
  
  // execute the callback
  Local<Function>::New(isolate, work->callback)->Call(isolate->GetCurrentContext()->Global(), argc, argv);
  
  // Free up the persistent function callback
  work->callback.Reset();
  delete work;
}

static void StopGif(uv_work_t *req) {
  fprintf(stderr, "StopGif\n");
  Work *work = static_cast<Work *>(req->data);
  showAnimation = false;
}

static void StopGifComplete(uv_work_t *req, int status) {
  fprintf(stderr, "StopGifComplete\n");
  Isolate * isolate = Isolate::GetCurrent();

  // Fix for Node 4.x - thanks to https://github.com/nwjs/blink/commit/ecda32d117aca108c44f38c8eb2cb2d0810dfdeb
  v8::HandleScope handleScope(isolate);
  Work *work = static_cast<Work *>(req->data);
  
  // set up return arguments
  const unsigned argc = 1;
  Local<Value> argv[argc] = { String::NewFromUtf8(isolate, "STOPPING GIF NOW") };
  
  // execute the callback
  Local<Function>::New(isolate, work->callback)->Call(isolate->GetCurrentContext()->Global(), argc, argv);
  
  // Free up the persistent function callback
  work->callback.Reset();
  delete work;
}

// called by libuv in event loop when async function completes
static void MainAsyncComplete(uv_work_t *req, int status) {
    fprintf(stderr, "MainAsyncComplete\n");
    Isolate * isolate = Isolate::GetCurrent();

    // Fix for Node 4.x - thanks to https://github.com/nwjs/blink/commit/ecda32d117aca108c44f38c8eb2cb2d0810dfdeb
    v8::HandleScope handleScope(isolate);
    Work *work = static_cast<Work *>(req->data);
    
    // set up return arguments
    const unsigned argc = 1;
    Local<Value> argv[argc] = { String::NewFromUtf8(isolate, "FINSIHED LOADING FRAMES") };
    
    // execute the callback
    Local<Function>::New(isolate, work->callback)->Call(isolate->GetCurrentContext()->Global(), argc, argv);
    
    // Free up the persistent function callback
    work->callback.Reset();
    delete work;
}

void CallAsyncMain(const v8::FunctionCallbackInfo<v8::Value>&args) {
    Isolate* isolate = args.GetIsolate();
    
    Work * work = new Work();
    work->request.data = work;
    
    //set the filename
    v8::String::Utf8Value nameFromArgs(args[0]->ToString());
    work->filename = std::string(*nameFromArgs);
    
    Local<Function> callback = Local<Function>::Cast(args[1]);
    work->callback.Reset(isolate, callback);
    
    uv_queue_work(uv_default_loop(),&work->request,MainAsync,MainAsyncComplete);
    fprintf(stderr, "CallAsyncMain\n");
    args.GetReturnValue().Set(Undefined(isolate));
}

void CallAsyncPlayGif(const v8::FunctionCallbackInfo<v8::Value>&args) {
    Isolate* isolate = args.GetIsolate();
    
    Work * work = new Work();
    work->request.data = work;
    
    Local<Function> callback = Local<Function>::Cast(args[0]);
    work->callback.Reset(isolate, callback);
    
    uv_queue_work(uv_default_loop(),&work->request,PlayGif,PlayGifComplete);
    args.GetReturnValue().Set(Undefined(isolate));
}

void CallAsyncStopGif(const v8::FunctionCallbackInfo<v8::Value>&args) {
    Isolate* isolate = args.GetIsolate();
    
    Work * work = new Work();
    work->request.data = work;
    
    Local<Function> callback = Local<Function>::Cast(args[0]);
    work->callback.Reset(isolate, callback);
    
    uv_queue_work(uv_default_loop(),&work->request,StopGif,StopGifComplete);
    args.GetReturnValue().Set(Undefined(isolate));
}

void init(Handle <Object> exports, Handle<Object> module) {
  NODE_SET_METHOD(exports, "start", CallAsyncMain);
  NODE_SET_METHOD(exports, "playGif", CallAsyncPlayGif);
  NODE_SET_METHOD(exports, "stopGif", CallAsyncStopGif);
}

NODE_MODULE(addon, init);