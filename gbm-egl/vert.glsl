attribute vec3 positionIn;

varying vec4 vVaryingColor;

void main()
{
  gl_Position = vec4(positionIn, 1);
  vVaryingColor = vec4(positionIn, 1);
}
