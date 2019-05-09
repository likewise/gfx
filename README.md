This are incremental experiments working towards a
proof-of-concept of rendering audio volume unit
metering bars, composited on top of other content,
hardware-accelerated using OpenGL to program the GPU,
and offscreen buffers through EGL and GBM.

The source code is forked from an existing project that
already targetted EGL and GBM, called yuq-gfx on github.

Leon Woestenberg <leon@sidebranch.com>

The incremental experiments are as follows

gbm-egl
gbm-egl-performance
gbm-egl-streaming   (see if persistent streaming buffers)
gbm-egl-compositing (blend on top of a RGBA texture from PNG)
