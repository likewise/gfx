precision mediump float;

varying vec4 vVaryingColor;

void main() {
#if 1
	if (vVaryingColor.x < 0.0) {
		gl_FragColor = vec4(1.0, 1.0, 1.0, 1);
	} else {
		gl_FragColor = vVaryingColor;
	}
#else
	gl_FragColor = vec4(1.0, 0.0, 0.0, 1);
#endif
}
