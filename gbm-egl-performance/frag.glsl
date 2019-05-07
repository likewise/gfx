#version 300 es
precision mediump float;

in vec3 vVaryingColor;

out vec4 fragColor;

void main() {
#if 0
	if (vVaryingColor.x < 0.0) {
		fragColor = vec4(1.0, 1.0, 1.0, 0.5);
	} else {
		fragColor = vec4(vVaryingColor, 0.5);
	}
#else
		fragColor = vec4(0.5, 0.5, 1.0, 0.9);
#endif
}
