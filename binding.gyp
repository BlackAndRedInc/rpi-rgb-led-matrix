{
    "targets": [
        {
            "target_name": "addon",
            "sources": [
                "led-image-viewer.cc",
                "lib/bdf-font.cc",
                "lib/framebuffer.cc", 
                "lib/gpio.cc",
                "lib/graphics.cc",
                "lib/led-matrix.cc",
                "lib/thread.cc",
                "lib/transformer.cc"
            ],
            "libraries": [
                "-lrt", 
                "-lm",
                "-lpthread"
            ],
            "defines": [
                "RGB_SLOWDOWN_GPIO=1",
                "ADAFRUIT_RGBMATRIX_HAT"
            ],
            "include_dirs": [
                "./include/"
            ],
            "link_settings": {
                "libraries": [ "<!(GraphicsMagick++-config --libs)" ]
            },
            "ldflags": [ 
                "<!(GraphicsMagick++-config --ldflags)"
            ],
            "cflags_cc": [
                "<!(GraphicsMagick++-config --cppflags)",
                "<!(GraphicsMagick++-config --cxxflags)" 
            ]
        }
    ]
}