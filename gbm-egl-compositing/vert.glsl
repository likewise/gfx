attribute vec2 inVertexPos;
attribute vec4 inVertexCol;
//attribute vec2 inTexCoord;

varying vec4 outVertexCol;
varying vec2 outTexCoord;

uniform mat4 orthoView;

void main()
{
   gl_Position = orthoView * vec4(inVertexPos, 1.0, 1.0);
   outVertexCol = inVertexCol;
   //outTexCoord = inTexCoord;
   // GL coords are in [-1,1], texture coordinates are in [0,1]
   outTexCoord = vec2((gl_Position.x + 1.0f) / 2.0f, (gl_Position.y + 1.0f) / 2.0f);
}
