#version 300 es
precision mediump float;

in vec4 vVaryingColor;

out vec4 fragColor;

void main() {
#if 1
	if (vVaryingColor.x < 0.0) {
		fragColor = vec4(1.0, 1.0, 1.0, 1);
	} else {
		fragColor = vVaryingColor;
	}
#else
	fragColor = vec4(1.0, 0.0, 0.0, 1);
#endif
}
