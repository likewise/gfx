#version 300 es
in vec3 positionIn;

out vec3 vVaryingColor;

void main()
{
  gl_Position = vec4(positionIn, 1);
  vVaryingColor = positionIn;
}
