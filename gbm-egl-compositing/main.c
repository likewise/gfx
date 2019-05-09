// clock_gettime >= 199309, posix_memalign >= 200112L
#define _POSIX_C_SOURCE 200112L //

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <png.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

GLuint program;
EGLDisplay display;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context;
struct gbm_device *gbm;
struct gbm_surface *gs;

static const size_t appWidth = 1920 * 4;
static const size_t appHeight = 1080 * 4;

static const float rectWidth = 30;
static const float rectHeight = 100;

// comment-out to allocate our own FBO -- improves render performance, unknown why yet
#define USE_EGL_SURFACE
//#define USE_DYNAMIC_STREAMING
#define SPRITE_COUNT 2048*5
static float gravity = 1.5f;

/* subtracts t2 from t1, the result is in t1
 * t1 and t2 should be already normalized, i.e. nsec in [0, 1000000000)
 */
static void timespec_sub(struct timespec *t1, const struct timespec *t2)
{
  assert(t1->tv_nsec >= 0);
  assert(t1->tv_nsec < 1000000000);
  assert(t2->tv_nsec >= 0);
  assert(t2->tv_nsec < 1000000000);
  t1->tv_sec -= t2->tv_sec;
  t1->tv_nsec -= t2->tv_nsec;
  if (t1->tv_nsec >= 1000000000)
  {
    t1->tv_sec++;
    t1->tv_nsec -= 1000000000;
  }
  else if (t1->tv_nsec < 0)
  {
    t1->tv_sec--;
    t1->tv_nsec += 1000000000;
  }
}

/* find first EGL configuration offering 32-bit buffer */
EGLConfig get_config(void)
{
  EGLint egl_config_attribs[] = {
    EGL_BUFFER_SIZE,  32,
    EGL_DEPTH_SIZE,   EGL_DONT_CARE,
    EGL_STENCIL_SIZE, EGL_DONT_CARE,
    EGL_RENDERABLE_TYPE,  EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE,
  };

  EGLint num_configs;
  assert(eglGetConfigs(display, NULL, 0, &num_configs) == EGL_TRUE);
  if (num_configs == 0) return 0;

  EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
  assert(eglChooseConfig(display, egl_config_attribs,
           configs, num_configs, &num_configs) == EGL_TRUE);
  assert(num_configs);
  printf("num config %d\n", num_configs);

  // Find a config whose native visual ID is the desired GBM format.
  for (int i = 0; i < num_configs; ++i) {
    EGLint gbm_format;

    assert(eglGetConfigAttrib(display, configs[i],
            EGL_NATIVE_VISUAL_ID, &gbm_format) == EGL_TRUE);
    printf("gbm format %x\n", gbm_format);

    if (gbm_format == GBM_FORMAT_ARGB8888) {
      /* copy chosen configuration from the array */
      EGLConfig ret = configs[i];
      free(configs);
      return ret;
    }
  }

  // Failed to find a config with matching GBM format.
  abort();
}

void RenderTargetInit(void)
{
  int fd = open("/dev/dri/renderD128", O_RDWR);
  assert(fd >= 0);

  gbm = gbm_create_device(fd);
  assert(gbm != NULL);

  display = eglGetDisplay(gbm);
  assert(display  != EGL_NO_DISPLAY);

  EGLint majorVersion;
  EGLint minorVersion;
  EGLBoolean egl_rc;

#if 1
  int epoxy_egl_ver = epoxy_egl_version(display);
  printf("libepoxy says %d\n", epoxy_egl_ver);
#endif

  egl_rc = eglInitialize(display, &majorVersion, &minorVersion);
  assert(egl_rc == EGL_TRUE);

  char const *egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
  if (egl_extensions) printf("%s\n", egl_extensions);

  egl_rc = eglBindAPI(EGL_OPENGL_ES_API);
  assert(egl_rc == EGL_TRUE);

  /* no native EGL surface, requires InitFBO to attach framebuffer */
  surface = EGL_NO_SURFACE;
  EGLConfig config = NULL;

  /* EGL can work without a native surface; in that case we initialize a
   * framebuffer object (FBO) ourselves in InitFBO(). In case of an EGL
   * surface, choose a configuration */
#ifdef USE_EGL_SURFACE
  config = get_config();
#endif
  if (config) {
    /* GBM surface */
    gs = gbm_surface_create(
      gbm, appWidth, appHeight, GBM_BO_FORMAT_ARGB8888,
#if 0
      /* non-tiled, sub-optimal for performance */
      GBM_BO_USE_LINEAR |
#endif
#if 0
      GBM_BO_USE_SCANOUT |
#endif
#if 0
      GBM_BO_USE_RENDERING |
#endif
      0);
    assert(gs);

    surface = eglCreatePlatformWindowSurfaceEXT(display, config, gs, NULL);
    assert(surface != EGL_NO_SURFACE);
  }

  const EGLint contextAttribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };

  context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
  assert(context != EGL_NO_CONTEXT);

  /* OES_surfaceless_context */
  egl_rc = eglMakeCurrent(display, surface, surface, context);
  assert(egl_rc == EGL_TRUE);
}

GLuint LoadShader(const char *name, GLenum type)
{
  FILE *f;
  int size;
  char *buff;
  GLuint shader;
  GLint compiled;
  const GLchar *source[1];

  assert((f = fopen(name, "r")) != NULL);

  // get file size
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);

  assert((buff = malloc(size)) != NULL);
  assert(fread(buff, 1, size, f) == size);
  source[0] = buff;
  fclose(f);
  shader = glCreateShader(type);
  glShaderSource(shader, 1, source, &size);
  glCompileShader(shader);
  free(buff);

  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLint infoLen = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen > 1) {
      char *infoLog = malloc(infoLen);
      glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
      fprintf(stderr, "Error compiling shader %s:\n%s\n", name, infoLog);
      free(infoLog);
    }
    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

#if 0
struct shaders_t {
  GLuint vertexShader;
  GLuint fragmentShader;
  GLuint program;
};

struct shaders_t *createShaderProgram(const char *vertex_glsl_filename, const char *fragment_glsl_filename) {
  struct shaders_t *shaders = malloc(sizeof(struct shaderprogram_t));
  assert(shaders);
  shaders.vertexShader = LoadShader(vertex_glsl_filename, GL_VERTEX_SHADER);
  assert(shaders.vertexShader);
  shaders.fragmentShader = LoadShader(fragment_glsl_filename, GL_FRAGMENT_SHADER);
  assert(shaders.vertexShader);
  shaders.program = glCreateProgram();
  assert(shaders.program);
  glAttachShader(shaders.program, vertexShader);
  glAttachShader(shaders.program, fragmentShader);
  glLinkProgram(shaders.program);

  /* verify linking was succesful */
  GLint linked;
    glGetProgramiv(shaders.program, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint infoLen = 0;
    glGetProgramiv(shaders.program, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen > 1) {
      char *infoLog = malloc(infoLen);
      glGetProgramInfoLog(program, infoLen, NULL, infoLog);
      fprintf(stderr, "Error linking program:\n%s\n", infoLog);
      free(infoLog);
    }
    glDeleteProgram(shaders.program);
    exit(1);
  }
}
#endif

void CheckFrameBufferStatus(void)
{
  GLenum status;
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  switch(status) {
  case GL_FRAMEBUFFER_COMPLETE:
    printf("Framebuffer complete\n");
    break;
  case GL_FRAMEBUFFER_UNSUPPORTED:
    printf("Framebuffer unsupported\n");
    break;
  case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
    printf("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
    break;
  case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
    printf("GL_FRAMEBUFFER_MISSING_ATTACHMENT\n");
    break;
  case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
    printf("GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS\n");
    break;
  default:
    printf("Framebuffer error\n");
  }
}

void CheckError(void)
{
  GLenum error;
  error = glGetError();
  switch(error) {
  case GL_NO_ERROR:
    break;
  case GL_FRAMEBUFFER_UNSUPPORTED:
    printf("Framebuffer unsupported\n");
    break;
  case GL_INVALID_ENUM:
    printf("An unacceptable value is specified for an enumerated argument. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_VALUE:
    printf("A numeric argument is out of range. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_OPERATION:
    printf("The specified operation is not allowed in the current state. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_FRAMEBUFFER_OPERATION:
    printf("The framebuffer object is not complete. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_OUT_OF_MEMORY:
    printf("There is not enough memory left to execute the command. The state of the GL is undefined, except for the state of the error flags, after this error is recorded.\n");
    break;
  case GL_STACK_UNDERFLOW:
    printf("An attempt has been made to perform an operation that would cause an internal stack to underflow.\n");
    break;
  case GL_STACK_OVERFLOW:
    printf("An attempt has been made to perform an operation that would cause an internal stack to overflow. \n");
    break;
  default:
    printf("Unknown error\n");
  }
  assert(error == GL_NO_ERROR);
}

void InitFBO(void)
{
  struct gbm_bo *bo = gbm_bo_create(gbm, appWidth, appHeight,
                     GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT
                     /*GBM_BO_USE_LINEAR*/
                     /*GBM_BO_USE_SCANOUT*/);
  EGLImageKHR image = eglCreateImageKHR(display, context,
                      EGL_NATIVE_PIXMAP_KHR, bo, NULL);
  assert(image != EGL_NO_IMAGE_KHR);

  GLuint texid;
  glGenTextures(1, &texid);
  glBindTexture(GL_TEXTURE_2D, texid);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

  GLuint fbid;
  glGenFramebuffers(1, &fbid);
  glBindFramebuffer(GL_FRAMEBUFFER, fbid);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texid, 0);
/*
  GLuint rbid;
  glGenRenderbuffers(1, &rbid);
  glBindRenderbuffer(GL_RENDERBUFFER, rbid);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, appWidth, appHeight);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbid);
//*/
  CheckFrameBufferStatus();
}

/* @TODO glDeleteShader(), glDeleteProgram() */
void InitGLES(void)
{
  GLint linked;
  GLuint vertexShader;
  GLuint fragmentShader;
  vertexShader = LoadShader("vert.glsl", GL_VERTEX_SHADER);
  assert(vertexShader != 0);
  fragmentShader = LoadShader("frag.glsl", GL_FRAGMENT_SHADER);
  assert(fragmentShader  != 0);
  program = glCreateProgram();
  assert(program  != 0);
  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glLinkProgram(program);
  /* verify linking was succesful */
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint infoLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen > 1) {
      char *infoLog = malloc(infoLen);
      glGetProgramInfoLog(program, infoLen, NULL, infoLog);
      fprintf(stderr, "Error linking program:\n%s\n", infoLog);
      free(infoLog);
    }
    glDeleteProgram(program);
    exit(1);
  }

  if (surface == EGL_NO_SURFACE) {
    printf("No native EGL surface, allocating FBO.\n");
    InitFBO();
  }

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glViewport(0, 0, appWidth, appHeight);

  //glEnable(GL_DEPTH_TEST);

  glUseProgram(program);
}

int writeImage(char* filename, int width, int height, void *buffer, char* title)
{
  int code = 0;
  FILE *fp = NULL;
  png_structp png_ptr = NULL;
  png_infop info_ptr = NULL;

  // Open file for writing (binary mode)
  fp = fopen(filename, "wb");
  if (fp == NULL) {
    fprintf(stderr, "Could not open file %s for writing\n", filename);
    code = 1;
    goto finalise;
  }

  // Initialize write structure
  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (png_ptr == NULL) {
    fprintf(stderr, "Could not allocate write struct\n");
    code = 1;
    goto finalise;
  }

  // Initialize info structure
  info_ptr = png_create_info_struct(png_ptr);
  if (info_ptr == NULL) {
    fprintf(stderr, "Could not allocate info struct\n");
    code = 1;
    goto finalise;
  }

  // Setup Exception handling
  if (setjmp(png_jmpbuf(png_ptr))) {
    fprintf(stderr, "Error during png creation\n");
    code = 1;
    goto finalise;
  }

  png_init_io(png_ptr, fp);

  // Write header (8 bit colour depth)
  png_set_IHDR(png_ptr, info_ptr, width, height,
         8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
         PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  // Set title
  if (title != NULL) {
    png_text title_text;
    title_text.compression = PNG_TEXT_COMPRESSION_NONE;
    title_text.key = "Title";
    title_text.text = title;
    png_set_text(png_ptr, info_ptr, &title_text, 1);
  }

  png_write_info(png_ptr, info_ptr);

  // Write image data
  int i;
  for (i = 0; i < height; i++)
    png_write_row(png_ptr, (png_bytep)buffer + i * width * 4);

  // End write
  png_write_end(png_ptr, NULL);

 finalise:
  if (fp != NULL) fclose(fp);
  if (info_ptr != NULL) png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
  if (png_ptr != NULL) png_destroy_write_struct(&png_ptr, (png_infopp)NULL);

  return code;
}

float random_float(float min, float max)
{
  float range = max - min;
  /* obtain random float in range [0.0 - 1.0] */
  float value = (float)rand() / ((float)RAND_MAX + 1.0);
  value *= range;
  value += min;
  value += (float)rand() / ((float)RAND_MAX + 1.0) / ((float)RAND_MAX + 1.0);

  return value;
}

struct particles_t
{
   __attribute__((aligned(32)))
  float positionX[SPRITE_COUNT];
   __attribute__((aligned(32)))
  float positionY[SPRITE_COUNT];
   __attribute__((aligned(32)))
  float velocityX[SPRITE_COUNT];
   __attribute__((aligned(32)))
  float velocityY[SPRITE_COUNT];

  float colorR[SPRITE_COUNT];
  float colorG[SPRITE_COUNT];
  float colorB[SPRITE_COUNT];
  size_t count;
};

/* initialize particle positions, velocities and colours */
void constructParticles(struct particles_t *particles)
{
  for (size_t index = 0; index < SPRITE_COUNT; ++index)
  {
    particles->positionX[index] = random_float(0, appWidth); //appWidth / 2;
    particles->positionY[index] = random_float(0, appHeight); //appHeight / 2;
    particles->velocityX[index] = random_float(5, 10) * cosf(2 * 3.14 * index / SPRITE_COUNT);
    particles->velocityY[index] = random_float(5, 10) * sinf(2 * 3.14 * index / SPRITE_COUNT);
    particles->colorR[index] = random_float(0, 1);
    particles->colorG[index] = random_float(0, 1);
    particles->colorB[index] = random_float(0, 1);
  }
  particles->count = SPRITE_COUNT;
}

/* update particle positions */
void updateParticles(struct particles_t *particles)
{
  for (size_t index = 0; index < particles->count; ++index)
  {
    //particles->velocityY[index] += gravity;
    float excess;

    particles->positionY[index] += particles->velocityY[index];
    particles->positionX[index] += particles->velocityX[index];

    excess = particles->positionY[index] - (appHeight - 100);
    //if (particles->positionY[index] > appHeight - 100)
    if (excess > 0.0)
    {
      particles->positionY[index] = (appHeight - 100) - excess;
      particles->velocityY[index] *= -1.0f;
    }
    excess = particles->positionX[index] - (appWidth - 100);
    //if (particles->positionX[index] > appWidth - 100)
    if (excess > 0.0)
    {
      /* @TODO: physically incorrect, do not bound but mirror */
      particles->positionX[index] = (appWidth - 100) - excess;
      particles->velocityX[index] *= -1.0f;
    }
    excess = 100 - particles->positionX[index];
    //else if (particles->positionX[index] < 100)
    if (excess > 0.0)
    {
      particles->positionX[index] = 100 + excess;
      particles->velocityX[index] *= -1.0f;
    }
  }
}

static float colorR = 1.0f;
static float colorG = 1.0f;
static float colorB = 1.0f;
static float colorA = 1.0f;

void setColor(float r, float g, float b)
{
  colorR = r;
  colorG = g;
  colorB = b;
  colorA = 0.8f;
}

static size_t bufferDataIndex = 0;
static const size_t vertPerQuad = 6;
static const size_t maxVertices = SPRITE_COUNT * vertPerQuad;

static float *pVertexPosBufferData = NULL;
static float *pVertexPosCurrent = NULL;
static float *pVertexColBufferData = NULL;
static float *pVertexColCurrent = NULL;

void drawRect(float x, float y, float width, float height)
{
  // first triangle
  pVertexPosCurrent[0] = x;
  pVertexPosCurrent[1] = y;
  pVertexPosCurrent[2] = x + width;
  pVertexPosCurrent[3] = y + height;
  pVertexPosCurrent[4] = x;
  pVertexPosCurrent[5] = y + height;
  // second triangle
  pVertexPosCurrent[6] = x;
  pVertexPosCurrent[7] = y;
  pVertexPosCurrent[8] = x + width;
  pVertexPosCurrent[9] = y;
  pVertexPosCurrent[10] = x + width;
  pVertexPosCurrent[11] = y + height;
#if 0
  printf("%4.2f,%4.2f -> %4.2f,%4.2f", x, y, x + width, y + height);
  printf(" (%1.2f,%1.2f,%1.2f,%1.2f)\n", colorR, colorG, colorB, colorA);
#endif
  // first triangle
  pVertexColCurrent[0] = colorR;
  pVertexColCurrent[1] = colorG;
  pVertexColCurrent[2] = colorB;
  pVertexColCurrent[3] = colorA;

  pVertexColCurrent[4] = colorR;
  pVertexColCurrent[5] = colorG;
  pVertexColCurrent[6] = colorB;
  pVertexColCurrent[7] = colorA;

  pVertexColCurrent[8] = colorR;
  pVertexColCurrent[9] = colorG;
  pVertexColCurrent[10] = colorB;
  pVertexColCurrent[11] = colorA;

  // second triangle
  pVertexColCurrent[12] = colorR;
  pVertexColCurrent[13] = colorG;
  pVertexColCurrent[14] = colorB;
  pVertexColCurrent[15] = colorA;

  pVertexColCurrent[16] = colorR;
  pVertexColCurrent[17] = colorG;
  pVertexColCurrent[18] = colorB;
  pVertexColCurrent[19] = colorA;

  pVertexColCurrent[20] = colorR;
  pVertexColCurrent[21] = colorG;
  pVertexColCurrent[22] = colorB;
  pVertexColCurrent[23] = colorA;

  pVertexPosCurrent = (float *)((char *)pVertexPosCurrent + (sizeof(float) * 12));
  pVertexColCurrent = (float *)((char *)pVertexColCurrent + (sizeof(float) * 24));

  bufferDataIndex++;
}

void renderParticles(struct particles_t* particles)
{
  for (size_t index = 0; index < particles->count; ++index)
  {
    setColor(particles->colorR[index], particles->colorG[index], particles->colorB[index]);
    drawRect(particles->positionX[index], particles->positionY[index], rectWidth, rectHeight);
  }
}

static GLuint vertexPosVBO;
static GLuint vertexColVBO;
static GLuint locVertexPos;
static GLuint locVertexCol;
static GLbitfield allocFlag;

void flushBufferData0()
{
  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * bufferDataIndex * vertPerQuad * 2, pVertexPosBufferData);
  CheckError();
  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * bufferDataIndex * vertPerQuad * 4, pVertexColBufferData);
  CheckError();

  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(bufferDataIndex * vertPerQuad));
  CheckError();
  bufferDataIndex = 0;
  pVertexPosCurrent = pVertexPosBufferData;
  pVertexColCurrent = pVertexColBufferData;
}

/* USE_DYNAMIC_STREAMING */
void flushBufferData1()
{
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(bufferDataIndex * vertPerQuad));
  CheckError();
  bufferDataIndex = 0;
  pVertexPosCurrent = pVertexPosBufferData;
  pVertexColCurrent = pVertexColBufferData;
}

void flush()
{
#if defined(USE_DYNAMIC_STREAMING)
  flushBufferData1();
#else
  flushBufferData0();
#endif
}

void Render(void)
{
  srand((unsigned int)time(NULL));

  struct particles_t *particles = NULL;
  int rc = posix_memalign((void **)&particles, 32, sizeof(struct particles_t));
  assert(rc == 0);

  constructParticles(particles);

  CheckFrameBufferStatus();

  /* read RGBA into texture */
  uint8_t *data = NULL;
#if 0
  /* @TODO probably need to assert (w==appWidth) && (h==appHeight)
   * or either set the latter from the PNG very early before appWidth/Height are used */
  data = readImage("crate-base.png", &w, &h);
#else
  data = malloc(appWidth * appHeight * 4);
  assert(data);
  for (int i = 0; i < appWidth; i++) {
    for (int j = 0; j < appHeight; j++) {
      data[(j * appWidth + i) * 4 + 0] = 128;
      data[(j * appWidth + i) * 4 + 1] = 32;
      data[(j * appWidth + i) * 4 + 2] = 32;
      data[(j * appWidth + i) * 4 + 3] = 128;
      if (((i + j) % 8) == 0) data[(j * appWidth + i) * 4 + 3] = 255;
    }
  }
#endif

  glActiveTexture(GL_TEXTURE0);

  /* create a texture */
  GLuint texid = 0;
  glGenTextures(1, &texid);
  glBindTexture(GL_TEXTURE_2D, texid);
#if 0
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, appWidth, appHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  CheckError();

  /* create a framebuffer with the texture as color attachment */
  GLuint fbid;
  glGenFramebuffers(1, &fbid);
  glBindFramebuffer(GL_FRAMEBUFFER, fbid);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texid, 0);
  CheckError();

#ifndef USE_EGL_SURFACE
#error "Need to bind to the InitFBO framebuffer here."
#endif
  glBindFramebuffer(GL_FRAMEBUFFER, 0/*default framebuffer*/);

  // Setup 2D orthographic matrix view
  GLint locOrthoView = glGetUniformLocation(program, "orthoView");
  GLfloat ortho2D[16] = {
      2.0f / appWidth, 0, 0, 0,
      0, -2.0f / appHeight, 0, 0,
      0, 0, 1.0f, 1.0f,
      -1.0f, 1.0f, 0, 0
  };
  glUniformMatrix4fv(locOrthoView, 1, GL_FALSE, ortho2D);

  GLint locVertexPos = glGetAttribLocation(program, "inVertexPos");
  GLint locVertexCol = glGetAttribLocation(program, "inVertexCol");

  // Generate and Allocate Buffers
  glGenBuffers(1, &vertexPosVBO);
  assert(glGetError() == GL_NO_ERROR);
  glGenBuffers(1, &vertexColVBO);
  assert(glGetError() == GL_NO_ERROR);

  size_t vpSize = SPRITE_COUNT * (sizeof(float) * 12);
  size_t vcSize = SPRITE_COUNT * (sizeof(float) * 24);

  /* buffer allocation */
#if defined(USE_DYNAMIC_STREAMING)
  GLbitfield mapFlags =
    GL_MAP_WRITE_BIT |
    GL_MAP_PERSISTENT_BIT |
    GL_MAP_COHERENT_BIT;
  GLbitfield createFlags = mapFlags | GL_DYNAMIC_STORAGE_BIT;

  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  glBufferStorage(GL_ARRAY_BUFFER, vpSize, NULL, createFlags);
  CheckError();
  glEnableVertexAttribArray(locVertexPos);
  CheckError();
  glVertexAttribPointer(locVertexPos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
  pVertexPosBufferData = (GLfloat *)glMapBufferRange(GL_ARRAY_BUFFER, 0, vpSize, mapFlags);
  pVertexPosCurrent = pVertexPosBufferData;

  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  glBufferStorage(GL_ARRAY_BUFFER, vcSize, NULL, createFlags);
  CheckError();
  glEnableVertexAttribArray(locVertexCol);
  CheckError();
  glVertexAttribPointer(locVertexCol, 4, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
  pVertexColBufferData = (GLfloat *)glMapBufferRange(GL_ARRAY_BUFFER, 0, vcSize, mapFlags);
  pVertexColCurrent = pVertexColBufferData;

#else
  pVertexPosBufferData = (GLfloat *)malloc(vpSize);
  assert(pVertexPosBufferData);
  pVertexColBufferData = (GLfloat *)malloc(vcSize);
  assert(pVertexColBufferData);
  pVertexPosCurrent = pVertexPosBufferData;
  pVertexColCurrent = pVertexColBufferData;

  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  glBufferData(GL_ARRAY_BUFFER, vpSize, NULL, GL_DYNAMIC_DRAW);
  CheckError();
  glEnableVertexAttribArray(locVertexPos);
  glVertexAttribPointer(locVertexPos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();

  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  glBufferData(GL_ARRAY_BUFFER, vcSize, NULL, GL_DYNAMIC_DRAW);
  CheckError();
  glEnableVertexAttribArray(locVertexCol);
  glVertexAttribPointer(locVertexCol, 4, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
#endif

  glClearColor(0, 0, 0, 1);

  renderParticles(particles);

  struct timespec ts_start, ts_end;
  rc = clock_gettime(CLOCK_MONOTONIC, &ts_start);

  int frame = 0;
  int num_frames = 3000;
  int endless = 0;
  //printf("Rendering %d frames.\n", num_frames);

  struct timespec ts_frame_start, ts_frame_end;
  rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_frame_start);

  while ((frame < num_frames) | endless) {

  /* blit a background image */
#if 1
    /* read from background texture, draw into surface */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbid);
    CheckError();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    CheckError();
    /* default, blit whole screen */
    int blit_count = 1;
    GLint x = 0;
    GLint y = 0;
    GLint w = appWidth;
    GLint h = appHeight;
#if 1
    /* incremental blits */
    if (frame > 0) {
      w = h = 400;
      blit_count = 20;
    }
#endif
    for (int i = 0; i < blit_count; i++) {
      /* the read and draw framebuffers are those bound to the
       * GL_READ_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER targets respectively. */
      glBlitFramebuffer(
        x, y, x + w, x + h, /*source rectangle */
        x, y, x + w, x + h, /*destination rectangle */
        GL_COLOR_BUFFER_BIT,
        GL_LINEAR/*no scaling anyway*/);
      CheckError();
      x += 10;
      y += 10;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    CheckError();
#else
    glClear(GL_COLOR_BUFFER_BIT /*| GL_DEPTH_BUFFER_BIT*/);
    CheckError();
#endif

#if 0
    /* update physics */
    updateParticles(particles);
    /* update vertices */
    renderParticles(particles);
#endif

#if 1
    /* render */
    flush();
#endif

    if (surface == EGL_NO_SURFACE) {
      /* glFlush() ensures all commands are on the GPU */
      /* glFinish() ensures all commands are also finished */
      glFinish();
    } else {
      eglSwapBuffers(display, surface);
    }
    frame++;

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_frame_end);
    struct timespec ts_frame_start_next = ts_frame_end;
    /* subtract the start time from the end time */
    timespec_sub(&ts_frame_end, &ts_frame_start);
    ts_frame_start = ts_frame_start_next;
    if (ts_frame_end.tv_sec > 0) {
      printf("frame took longer than 1 second?\n");
    }

    if (!(frame % 60)) {
      printf("frame %d in %3.2f ms (fps = %3.2f)\n", frame,
      (float)ts_frame_end.tv_nsec / 1000000.0f, 1000000000.0f / (float)ts_frame_end.tv_nsec);
    }
  }

  rc = clock_gettime(CLOCK_MONOTONIC, &ts_end);
  /* subtract the start time from the end time */
  timespec_sub(&ts_end, &ts_start);
  printf("CLOCK_MONOTONIC reports %ld.%09ld seconds\n",
    ts_end.tv_sec, ts_end.tv_nsec);

#ifndef USE_DYNAMIC_STREAMING
  free(pVertexPosBufferData);
  free(pVertexColBufferData);
#endif

  glDeleteBuffers(1, &vertexPosVBO);
  glDeleteBuffers(1, &vertexColVBO);

  free(particles); particles = NULL;

  GLubyte *result;
  result = malloc(appWidth * appHeight * 4);
  assert(result);
  printf("glFinish()\n");
  glFinish();
  printf("glReadPixels()\n");
  glReadPixels(0, 0, appWidth, appHeight, GL_RGBA, GL_UNSIGNED_BYTE, result);
  assert(glGetError() == GL_NO_ERROR);
  printf("writeImage()\n");
  assert(!writeImage("screenshot.png", appWidth, appHeight, result, "hello"));
}

int main(void)
{
  RenderTargetInit();
  InitGLES();
  Render();
  return 0;
}


