attribute vec2 inVertexPos;
attribute vec4 inVertexCol;
varying vec4 outVertexCol;
uniform mat4 orthoView;

void main()
{
   gl_Position = orthoView * vec4(inVertexPos, 1.0, 1.0);
   outVertexCol = inVertexCol;
}
