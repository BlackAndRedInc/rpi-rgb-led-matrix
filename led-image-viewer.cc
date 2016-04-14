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

#include <vector>
#include <Magick++.h>
#include <magick/image.h>

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
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);
  fprintf(stderr, "Display.\n");
  for (unsigned int i = 0; !interrupt_received; ++i) {
    const PreprocessedFrame *frame = frames[i % frames.size()];
    matrix->SwapOnVSync(frame->canvas());
    if (frames.size() == 1 || (play_once == true && i == frames.size() - 1)) {
      sleep(86400);  // Only one image. Nothing to do.
    } else {
      usleep(frame->delay_micros());
    }
  }
}

void Main(const FunctionCallbackInfo<Value>& args) {
  Magick::InitializeMagick("");

  int rows = 32;
  int chain = 4;
  int parallel = 1;
  int brightness = 90;
  bool play_once = false;

  const char *filename = "../gifs/idle.gif";

  /*
   * Set up GPIO pins. This fails when not running as root.
   */
  GPIO io;
  io.Init();
  assert(io.Init());
  RGBMatrix *const matrix = new RGBMatrix(&io, rows, chain, parallel);
  matrix->SetBrightness(brightness);
  
  std::vector<Magick::Image> sequence_pics;
  if (!LoadAnimation(filename, matrix->width(), matrix->height(),
                     &sequence_pics)) {
    return;
  }

  std::vector<PreprocessedFrame*> frames;
  PrepareBuffers(sequence_pics, matrix, &frames);
  DisplayAnimation(frames, matrix, play_once);
  
  matrix->Clear();
  delete matrix;
}

/*void Method(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "world"));
}*/

void init(Local<Object> exports) {
  NODE_SET_METHOD(exports, "start", Main);
}

NODE_MODULE(addon, init);