// Fragment Shader for Neuron Multiviewer OpenGL GPU rendering
// 2019 Leon Woestenberg <leon@sidebranch.com>
//

precision mediump float;
varying vec4 outVertexCol;

uniform sampler2D texId;
varying vec2 outTexCoord;

// NOTE: if you are not using a certain 'varying', or even multiplying it with 0.0f,
// it gets discarded from the shader program and you might get run-time OpenGL errors
// with GL_INVALID_VALUE
//
// NOTE: Use floats, i.e. 1.0 instead of 1, otherwise expect shader compiler error
// NOTE: Do not use float suffixes in GSLS 1.10

// output.rgb = background.rgb*(1 - foreground.a) + foreground.rgb

void main()
{
  vec4 texel = texture2D(texId, outTexCoord);

  // opacity of vertex;
  // what then remains visible of the background texture is (1.0 - opacity)
  float vtxOpacity = outVertexCol.a;

  vec3 texCol = texel.rgb;
  // texel colour is non-premultiplied (from PNG RGBA); multiply colour with its own alpha
  // @TODO verify, this depends on the Qt renderer!
  //texCol *= vec3(texel.a);

  vec3 vtxCol = outVertexCol.rgb;
  // vertex colour is non-premultiplied (from PNG RGBA), multiply colour it with its own alpha
  vtxCol *= vec3(outVertexCol.a);

  // texCol and vtxCol (now) contain pre-multiplied rgb
  // now blend the vertex over the texture

  // NOTE: OpenGL can also blend transparent pixels drawn to the draw buffer, this is independent
  // of what we do here.

  // Porter-Duff Over operator; alpha means pixel coverage
  // Calculate the resulting alpha value
  // as = alpha source
  // ad = alpha destination
  // The formula can be simplified:
  // as*(1-ad) + ad*(1-as) + as*ad = as - as*ad +ad - ad*as + as*ad = as + ad - as*ad
  float opacity = texel.a + outVertexCol.a - texel.a * outVertexCol.a;

  gl_FragColor = vec4(vec3(1.0 - vtxOpacity) * texCol + vtxCol, opacity);
  
  // for testing purposes only
  //gl_FragColor = vec4(vec3(1.0 - vtxOpacity) * texCol + vtxCol, texel.a);
  //gl_FragColor = vec4(vec3(1.0 - vtxOpacity) * texCol + vtxCol, 1.0);
}
