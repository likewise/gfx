attribute vec3 inVertexPos;
attribute vec4 inVertexCol;
//attribute vec2 inTexCoord;

varying vec4 outVertexCol;
varying vec2 outTexCoord;

uniform mat4 orthoView;

void main()
{
   //vec4 new_pos = orthoView * vec4(inVertexPos.xyz, 1.0);
   vec4 new_pos = orthoView * vec4(inVertexPos.xy, 1.0, 1.0);
   //gl_Position = new_pos;
   gl_Position = vec4(new_pos.xy, inVertexPos.z, 1.0);

   // pass vertex colour as-is
   outVertexCol = inVertexCol;

   //outTexCoord = inTexCoord;
   // GL coords are in [-1,1], texture coordinates are in [0,1]
   outTexCoord = vec2((gl_Position.x + 1.0f) / 2.0f, (gl_Position.y + 1.0f) / 2.0f);
}
