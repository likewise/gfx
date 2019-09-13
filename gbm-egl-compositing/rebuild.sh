#!/bin/sh
../../temp/run.do_compile && \
sudo cp -a gbm-egl-compositing /nfsroot/smarc/usr/bin/ &&
sudo cp -a frag.glsl /nfsroot/smarc/usr/share/gbm-egl-compositing/frag.glsl &&
sudo cp -a vert.glsl /nfsroot/smarc/usr/share/gbm-egl-compositing/vert.glsl
 
