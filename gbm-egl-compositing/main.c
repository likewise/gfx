// clock_gettime >= 199309, posix_memalign >= 200112L
#define _POSIX_C_SOURCE 200112L //
#define _XOPEN_SOURCE 500 // usleep
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <gbm.h>
#include <drm/drm_fourcc.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <png.h>

//#include <linux/ioctl.h>
#define IOCTL_XDMA_IMPORT_DMABUF    _IOW('q', 7, int)

GLuint program;
EGLDisplay display;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context;
struct gbm_device *gbm;
struct gbm_surface *gs;
struct gbm_bo *previous_bo = NULL;

/* Scaling factor against high definition (HD, 1920x1080).
 * Used both vertically and horizontally.
 * The resulting number of pixels is SCALE*SCALE*1920*1080.
 */
#define SCALE 4

// comment-out to allocate our own FBO -- improves render performance, unknown why yet
#define USE_EGL_SURFACE
#define USE_DYNAMIC_STREAMING
#define MAX_METERS 128 //(512*4)
#define NUM_RECT 4
#define MAX_RECTS (MAX_METERS * NUM_RECT)

static const size_t appWidth = 1920 * SCALE;
static const size_t appHeight = 1080 * SCALE;

#define NUM_BUFS 3
int buf_id = 0;

/* two triangles, each three vertices, each two coordinates */
size_t vertexPosSize = MAX_RECTS * (sizeof(float) * 2 * 3 * 3);
/* two triangles, each three vertices, each four colour components */
size_t vertexColSize = MAX_RECTS * (sizeof(float) * 2 * 3 * 4);

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
    EGL_BUFFER_SIZE, 32,
#if 1
    EGL_DEPTH_SIZE, 8,
#else
    EGL_DEPTH_SIZE, EGL_DONT_CARE,
#endif
    EGL_STENCIL_SIZE, EGL_DONT_CARE,
    //EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    /* The colour buffer will only be preserved over a swapbuffer if the
     * config used to create the surface has EGL_SWAP_BEHAVIOR_PRESERVED_BIT
     * set in EGL_SURFACE_TYPE, and EGL_SWAP_BEHAVIOR is set to EGL_BUFFER_PRESERVED
     * with eglSurfaceAttrib(). The default state is implementation dependent.
     */
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT/* | EGL_SWAP_BEHAVIOR_PRESERVED_BIT*/,
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
  //int fd = open("/dev/dri/card0", O_RDWR);
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
  printf("epoxy_egl_version()=%d\n", epoxy_egl_ver);
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
   * framebuffer object (FBO) ourselves in InitFBO().

   * In case of an EGL surface, choose a configuration here */
#ifdef USE_EGL_SURFACE
  config = get_config();
  assert(config);
#endif
  if (config) {
    /* GBM surface */
    gs = gbm_surface_create(
      gbm, appWidth, appHeight, GBM_BO_FORMAT_ARGB8888,
#if 0 // 3x slower
      /* non-tiled, sub-optimal for performance */
      GBM_BO_USE_LINEAR |
#else
      /* without linear, we expect and assert I915_FORMAT_MOD_X_TILED later */
#endif
#if 1
      GBM_BO_USE_SCANOUT |
#endif
#if 1
      GBM_BO_USE_RENDERING |
#endif
      0);
    assert(gs);

    surface = eglCreatePlatformWindowSurfaceEXT(display, config, gs, NULL);
    assert(surface != EGL_NO_SURFACE);
  }
  eglSurfaceAttrib(display, surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
  if (egl_rc == EGL_BAD_MATCH) {
    printf("BAD MATCH\n");
  }
  assert(egl_rc == EGL_TRUE);

  const EGLint contextAttribs[] = {
//    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE
  };

  context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
  assert(context != EGL_NO_CONTEXT);

  /* OES_surfaceless_context */
  egl_rc = eglMakeCurrent(display, surface, surface, context);
  assert(egl_rc == EGL_TRUE);

#ifdef USE_EGL_SURFACE
  egl_rc = eglSwapInterval(display, 1);
  assert(egl_rc == EGL_TRUE);
#endif
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
    printf("GL_FRAMEBUFFER_COMPLETE\n");
    break;
  case GL_FRAMEBUFFER_UNSUPPORTED:
    printf("GL_FRAMEBUFFER_UNSUPPORTED\n");
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
    printf("Unknown framebuffer error\n");
  }
}

/* a macro, such that the line number corresponds in the assertion */
#define CheckError(x) do { assert(_CheckError() == GL_NO_ERROR); } while (0)

GLenum _CheckError(void)
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
    printf("GL_INVALID_ENUM: An unacceptable value is specified for an enumerated argument. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_VALUE:
    printf("GL_INVALID_VALUE: A numeric argument is out of range. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_OPERATION:
    printf("GL_INVALID_OPERATION: The specified operation is not allowed in the current state. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_INVALID_FRAMEBUFFER_OPERATION:
    printf("GL_INVALID_FRAMEBUFFER_OPERATION: The framebuffer object is not complete. The offending command is ignored and has no other side effect than to set the error flag.\n");
    break;
  case GL_OUT_OF_MEMORY:
    printf("GL_OUT_OF_MEMORY: There is not enough memory left to execute the command. The state of the GL is undefined, except for the state of the error flags, after this error is recorded.\n");
    break;
  case GL_STACK_UNDERFLOW:
    printf("GL_STACK_UNDERFLOW: An attempt has been made to perform an operation that would cause an internal stack to underflow.\n");
    break;
  case GL_STACK_OVERFLOW:
    printf("GL_STACK_OVERFLOW: An attempt has been made to perform an operation that would cause an internal stack to overflow. \n");
    break;
  default:
    printf("Unknown error\n");
  }
  return error;
}

static GLuint our_fbo = 0;
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
  our_fbo = fbid;
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
  vertexShader = LoadShader("/usr/share/gbm-egl-compositing/vert.glsl", GL_VERTEX_SHADER);
  assert(vertexShader != 0);
  fragmentShader = LoadShader("/usr/share/gbm-egl-compositing/frag.glsl", GL_FRAGMENT_SHADER);
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

#if 0
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#else
  glDisable(GL_BLEND);
#endif

  glViewport(0, 0, appWidth, appHeight);

  glEnable(GL_DEPTH_TEST);

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

void *readImage(char *filename, int *width, int *height)
{
  char header[8];    // 8 is the maximum size that can be checked

  /* open file and test for it being a png */
  FILE *fp = fopen(filename, "rb");
  assert(fp);
  fread(header, 1, 8, fp);
  assert(!png_sig_cmp(header, 0, 8));

  /* initialize stuff */
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  assert(png_ptr);

  png_infop info_ptr = png_create_info_struct(png_ptr);
  assert(info_ptr);

  assert(!setjmp(png_jmpbuf(png_ptr)));

  png_init_io(png_ptr, fp);
  png_set_sig_bytes(png_ptr, 8);

  png_read_info(png_ptr, info_ptr);

  *width = png_get_image_width(png_ptr, info_ptr);
  *height = png_get_image_height(png_ptr, info_ptr);
  int color_type = png_get_color_type(png_ptr, info_ptr);
  assert(color_type == PNG_COLOR_TYPE_RGBA);
  int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
  assert(bit_depth == 8);
  int pitch = png_get_rowbytes(png_ptr, info_ptr);

  int number_of_passes = png_set_interlace_handling(png_ptr);
  png_read_update_info(png_ptr, info_ptr);

  /* read file */
  assert(!setjmp(png_jmpbuf(png_ptr)));

  png_bytep buffer = malloc(*height * pitch);
  void *ret = buffer;
  assert(buffer);
  png_bytep *row_pointers = malloc(sizeof(png_bytep) * *height);
  assert(row_pointers);
  for (int i = 0; i < *height; i++) {
    row_pointers[i] = buffer;
    buffer += pitch;
  }

  png_read_image(png_ptr, row_pointers);

  fclose(fp);
  free(row_pointers);
  return ret;
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

struct Meters_t
{
  float volume[MAX_METERS];
  float prev_volume[MAX_METERS];
  float hold[MAX_METERS];
  float prev_hold[MAX_METERS];
  int hold_time[MAX_METERS];
  size_t count;
};

struct Rectangles_t
{
   __attribute__((aligned(32)))
  float X1[MAX_RECTS];
   __attribute__((aligned(32)))
  float X2[MAX_RECTS];
   __attribute__((aligned(32)))
  float Y1[MAX_RECTS];
   __attribute__((aligned(32)))
  float Y2[MAX_RECTS];
   __attribute__((aligned(32)))
  float Z[MAX_RECTS];

  float colorR[MAX_RECTS];
  float colorG[MAX_RECTS];
  float colorB[MAX_RECTS];
  float colorA[MAX_RECTS];
  size_t count;
};

/* what percentage of pixels represent VU meters? */
#define VU_COVERAGE 25
#define VU_ROWS 4
#define HOR_METERS (MAX_METERS/VU_ROWS)
// 2 rectangles per meter
// percentage of what is available
#define VU_WIDTH (VU_COVERAGE * (appWidth/HOR_METERS) / 100)
#define VU_STRIDE (appWidth/HOR_METERS)

#define VU_TICK_HEIGHT (VU_WIDTH/2)

#define VU_HEIGHT ((appHeight/VU_ROWS) - appHeight/10)

/* initialize meter positions */
void constructMeters(struct Meters_t *Meters)
{
  for (size_t index = 0; index < MAX_METERS; index++)
  {

    Meters->volume[index] = 0.0;//random_float(0, VU_HEIGHT);
    if (index % 2) {
      Meters->volume[index] = 1;
    } else {
      Meters->volume[index] = VU_HEIGHT - 1;
    }
    Meters->prev_volume[index] = 0;
    Meters->hold[index] = 0.0;
    Meters->prev_hold[index] = 0;
    Meters->hold_time[index] = 0;
  }
}

/* initialize meter positions */
void updateMeters(struct Meters_t *Meters, int frame)
{
  for (size_t index = 0; index < MAX_METERS; index++)
  {
    Meters->prev_volume[index] = Meters->volume[index];
    Meters->prev_hold[index] = Meters->hold[index];

    if (index % 2) {
      Meters->volume[index] = 1 + (frame % 4) * VU_HEIGHT / 4; //random_float(0, VU_HEIGHT);
    } else {
      Meters->volume[index] = VU_HEIGHT - 1 - (frame % 4) * VU_HEIGHT / 4; //random_float(0, VU_HEIGHT);
    }

    Meters->volume[index] = random_float(0, VU_HEIGHT);

    /* keep track of hold tick presence time */
    Meters->hold_time[index]++;
    /* after 1 second, fall off hold tick */
    if (Meters->hold_time[index] > 6) {
      Meters->hold[index] /= 2;
    }
    /* if new maximum volume, reset hold tick */
    if (Meters->volume[index] >= Meters->hold[index]) {
      Meters->hold[index] = Meters->volume[index];
      Meters->hold_time[index] = 0;
    }
  }
}

void addRectanglesFromMeters(struct Rectangles_t *Rect, struct Meters_t *Meters)
{
  for (size_t meter = 0, rect = Rect->count; meter < MAX_METERS; meter++)
  {
    /* hold tick */
    if (Meters->hold[meter] > Meters->volume[meter]) {
      /* @TODO only draw if volume<hold */
      Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
      Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
      Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->hold[meter] - VU_TICK_HEIGHT;
      Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->hold[meter];
      /* tick never exceeds below base line */
      if (Rect->Y1[rect] < ((meter/HOR_METERS) * appHeight / VU_ROWS)) {
        //Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS;
      }
      /* in front of volume */
      Rect->Z[rect] = -0.1;
      Rect->colorR[rect] = 1.0;
      Rect->colorG[rect] = 0.0;
      Rect->colorB[rect] = 0.0;
      Rect->colorA[rect] = 0.5;
      rect++;
    }

#if 1 /* @TODO remove rendering active part of volume bar; should be already in background */
    /* volume bar */
    Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
    Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
    Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS;
    Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->volume[meter];
    Rect->Z[rect] = 0;
    Rect->colorR[rect] = 1.0;
    Rect->colorG[rect] = 1.0;
    Rect->colorB[rect] = 0.0;
    Rect->colorA[rect] = 0.5;
    rect++;
#endif
    /* all except volume bar */
    Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
    Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
    Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->volume[meter];
    Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + VU_HEIGHT;
    Rect->Z[rect] = 0;
    Rect->colorR[rect] = 0.0;
    Rect->colorG[rect] = 0.0;
    Rect->colorB[rect] = 0.0;
    Rect->colorA[rect] = 0.5;
    rect++;

#if 0
    printf("%3.2f, %3.2f, %3.2f, %3.2f\n", Rect->positionX[index], Rect->positionY[index],
      Rect->sizeX[index], Rect->sizeY[index]);
#endif
    Rect->count = rect;
  }
}

void addRectangle(struct Rectangles_t *Rect, float x1,  float y1, float x2, float y2, float z)
{
  size_t rect = Rect->count;
  Rect->X1[rect] = x1;
  Rect->Y1[rect] = y1;
  Rect->X2[rect] = x2;
  Rect->Y2[rect] = y2;
  Rect->Z[rect] = z;
  Rect->colorR[rect] = 1.0;
  Rect->colorG[rect] = 1.0;
  Rect->colorB[rect] = 1.0;
  Rect->colorA[rect] = 0.0;
  rect++;
  Rect->count = rect;
  assert(Rect->count < MAX_RECTS);
}

void addRectanglesFromMetersOptimized(struct Rectangles_t *Rect, struct Meters_t *Meters)
{
  for (size_t meter = 0, rect = Rect->count; meter < MAX_METERS; meter++)
  {
    if ((Meters->hold[meter] == Meters->prev_hold[meter]) &&
       (Meters->volume[meter] == Meters->prev_volume[meter]))
      continue;

    if (Meters->hold[meter] < Meters->prev_hold[meter]) {
      /* remove old hold tick */
      Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
      Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
      Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_hold[meter] - VU_TICK_HEIGHT;
      Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_hold[meter];
      Rect->colorR[rect] = 0.0;
      Rect->colorG[rect] = 0.0;
      Rect->colorB[rect] = 0.0;
      Rect->colorA[rect] = 0.5;
      rect++;
    } else if (Meters->hold[meter] > Meters->prev_hold[meter]) {
      /* remove old hold tick */
      Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
      Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
      Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_hold[meter] - VU_TICK_HEIGHT;
      Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_hold[meter];
      Rect->colorR[rect] = 1.0;
      Rect->colorG[rect] = 1.0;
      Rect->colorB[rect] = 0.0;
      Rect->colorA[rect] = 0.5;
      rect++;
    }

    /* volume bar higher? */
    if (Meters->volume[meter] > Meters->prev_volume[meter]) {
      /* draw the volume bar increase only */
      Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
      Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
      Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_volume[meter];
      Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->volume[meter];
      Rect->colorR[rect] = 1.0;
      Rect->colorG[rect] = 1.0;
      Rect->colorB[rect] = 1.0;
      Rect->colorA[rect] = 0.5;
      rect++;
    } else if (Meters->volume[meter] < Meters->prev_volume[meter]) {
      /* draw the volume bar decrease only */
      Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
      Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
      Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->volume[meter];
      Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->prev_volume[meter];
      Rect->colorR[rect] = 0.0;
      Rect->colorG[rect] = 0.0;
      Rect->colorB[rect] = 0.0;
      Rect->colorA[rect] = 0.5;
      rect++;
    }

    /* hold tick */
    Rect->X1[rect] = (meter % HOR_METERS) * VU_STRIDE;
    Rect->X2[rect] = (meter % HOR_METERS) * VU_STRIDE + VU_WIDTH;
    Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->hold[meter] - VU_TICK_HEIGHT;
    Rect->Y2[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS + Meters->hold[meter];
    /* tick never exceeds below base line */
    if (Rect->Y1[rect] < ((meter/HOR_METERS) * appHeight / VU_ROWS)) {
      //Rect->Y1[rect] = (meter/HOR_METERS) * appHeight / VU_ROWS;
    }
    Rect->colorR[rect] = 1.0;
    Rect->colorG[rect] = 0.0;
    Rect->colorB[rect] = 0.0;
    Rect->colorA[rect] = 0.5;
    rect++;
#if 0
    printf("%3.2f, %3.2f, %3.2f, %3.2f\n", Rect->positionX[index], Rect->positionY[index],
      Rect->sizeX[index], Rect->sizeY[index]);
#endif
    Rect->count = rect;
    assert(Rect->count < MAX_RECTS);
  }
}

static float colorR = 1.0f;
static float colorG = 1.0f;
static float colorB = 1.0f;
static float colorA = 1.0f;

void setColor(float r, float g, float b, float a)
{
  colorR = r;
  colorG = g;
  colorB = b;
  colorA = a;
}

static size_t numRects = 0;
static const size_t vertPerQuad = 6;
static const size_t maxVertices = MAX_RECTS * vertPerQuad;

static float *pVertexPosBufferData = NULL;
static float *pVertexColBufferData = NULL;

void drawRect(float x1, float y1, float x2, float y2, float z)
{
  /* pointer to float, six vertices each with x,y,z coords */
  float *pVertexPosCurrent = pVertexPosBufferData + (buf_id * MAX_RECTS + numRects) * 6 * 3;
  /* pointer to float, six vertices each with r,g,b,a components */
  float *pVertexColCurrent = pVertexColBufferData + (buf_id * MAX_RECTS + numRects) * 6 * 4;
  int i = 0;
  // first triangle (top-left half)
  pVertexPosCurrent[i++] = x1;
  pVertexPosCurrent[i++] = y1;
  pVertexPosCurrent[i++] = z;
  pVertexPosCurrent[i++] = x2;
  pVertexPosCurrent[i++] = y2;
  pVertexPosCurrent[i++] = z;
  pVertexPosCurrent[i++] = x1;
  pVertexPosCurrent[i++] = y2;
  pVertexPosCurrent[i++] = z;
  assert(i == (1 * 3 * 3));

  // second triangle (bottom-right half)
  pVertexPosCurrent[i++] = x1;
  pVertexPosCurrent[i++] = y1;
  pVertexPosCurrent[i++] = z;
  pVertexPosCurrent[i++] = x2;
  pVertexPosCurrent[i++] = y1;
  pVertexPosCurrent[i++] = z;
  pVertexPosCurrent[i++] = x2;
  pVertexPosCurrent[i++] = y2;
  pVertexPosCurrent[i++] = z;
  assert(i == (2 * 3 * 3));

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

  numRects++;
}

void clearRectangles(struct Rectangles_t* Rect)
{
  Rect->count = 0;
}

/* convert Rectangles into an OpenGL attribute arrays */
void tesselateRectangles(struct Rectangles_t* Rect)
{
  for (size_t index = 0; index < Rect->count; ++index)
  {
    setColor(Rect->colorR[index], Rect->colorG[index], Rect->colorB[index], Rect->colorA[index]);
    drawRect(Rect->X1[index], Rect->Y1[index], Rect->X2[index], Rect->Y2[index], Rect->Z[index]);
  }
}

static GLuint vertexPosVBO;
static GLuint vertexColVBO;
static GLuint locVertexPos;
static GLuint locVertexCol;
static GLbitfield allocFlag;

void flushBufferData()
{
  GLintptr offset;
  GLsizeiptr amount;

  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  offset = buf_id * MAX_RECTS * 6 * 3 * sizeof(float);
  amount = MAX_RECTS /*numRects*/ * 6 * 3 * sizeof(float);
  glBufferSubData(GL_ARRAY_BUFFER, offset, amount, pVertexPosBufferData + offset);
  CheckError();

  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  offset = buf_id * MAX_RECTS * 6 * 4 * sizeof(float);
  amount = MAX_RECTS /*numRects*/* 6 * 4 * sizeof(float);
  glBufferSubData(GL_ARRAY_BUFFER, offset, amount, pVertexColBufferData + offset);
  CheckError();

  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* USE_DYNAMIC_STREAMING */
void commitDraw()
{
  GLint first = buf_id * MAX_RECTS * vertPerQuad;
  glDrawArrays(GL_TRIANGLES, first, (GLsizei)(numRects * vertPerQuad));
  CheckError();

  buf_id = (buf_id + 1) % NUM_BUFS;
  numRects = 0;
}

void flushAndCommit()
{
#if !defined(USE_DYNAMIC_STREAMING)
  flushBufferData();
#endif
  commitDraw();
}

void Render(void)
{
  int rc;
  srand((unsigned int)time(NULL));

  /* meter state - each meter is a coloured rectangle */
  struct Meters_t *Meters = NULL;
  rc = posix_memalign((void **)&Meters, 32, sizeof(struct Meters_t));
  assert(rc == 0);

  struct Rectangles_t *Rect = NULL;
  rc = posix_memalign((void **)&Rect, 32, sizeof(struct Rectangles_t));
  assert(rc == 0);

  constructMeters(Meters);

  clearRectangles(Rect);
  addRectanglesFromMeters(Rect, Meters);

  CheckFrameBufferStatus();

  /* read RGBA into texture */
  uint8_t *data = NULL;

  /* verify correct alpha blending */
  int w = appWidth, h = appHeight;
#if 0
  data = readImage("AlphaBall.png", &w, &h);
  //assert(w == appWidth);
  //assert(h == appHeight);
#elif 1
  /* acp_app -platform synview >acp_app.log 2>&1 & */
  /* gbm-egl-compositing */
  int fd = open("/tmp/wom0", O_RDONLY);
  assert(fd >= 0);
  data = (uint8_t *)mmap(0, 1920*SCALE*1080*SCALE*4/*RGBA*/, PROT_READ, MAP_SHARED, fd, 0);
  assert(data);

  /* https://stackoverflow.com/questions/3887636/how-to-manipulate-texture-content-on-the-fly/10702468#10702468 */

#else
  /* generate synthetic background */
  data = malloc(appWidth * appHeight * 4);
  assert(data);
  for (int i = 0; i < appWidth; i++) {
    for (int j = 0; j < appHeight; j++) {
      data[(j * appWidth + i) * 4 + 0] = 128;
      data[(j * appWidth + i) * 4 + 1] = 32;
      data[(j * appWidth + i) * 4 + 2] = 32;
      data[(j * appWidth + i) * 4 + 3] = 255;
      if (((i + j) % 8) == 0) data[(j * appWidth + i) * 4 + 3] = 128;
    }
  }
  /* Red pixel */
  data[(0 * appWidth + 0) * 4 + 0] = 255;
  data[(0 * appWidth + 0) * 4 + 1] = 0;
  data[(0 * appWidth + 0) * 4 + 2] = 0;
  data[(0 * appWidth + 0) * 4 + 3] = 255;

  /* Green pixel */
  data[(0 * appWidth + 1) * 4 + 0] = 0;
  data[(0 * appWidth + 1) * 4 + 1] = 255;
  data[(0 * appWidth + 1) * 4 + 2] = 0;
  data[(0 * appWidth + 1) * 4 + 3] = 255;

  /* Blue pixel */
  data[(0 * appWidth + 2) * 4 + 0] = 0;
  data[(0 * appWidth + 2) * 4 + 1] = 0;
  data[(0 * appWidth + 2) * 4 + 2] = 255;
  data[(0 * appWidth + 2) * 4 + 3] = 255;

#endif

  glActiveTexture(GL_TEXTURE0);

  /* 
   * http://www.edlangley.co.uk/projects/opengl-streaming-textures/
   * "the trouble with using glTexImage2D to load the texture data, is
   * that the whole buffer is then copied to another memory location again"
   * 
   * GL_INTEL_map_texture 
   */

  /* create a texture from the data */
  GLuint texid = 0;
  glGenTextures(1, &texid);
  glBindTexture(GL_TEXTURE_2D, texid);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w/*appWidth*/, h/*appHeight*/, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  CheckError();

#if 1
  /* create a framebuffer with the texture as color attachment */
  GLuint fbid;
  glGenFramebuffers(1, &fbid);
  glBindFramebuffer(GL_FRAMEBUFFER, fbid);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texid, 0);
  CheckError();
#endif

#ifndef USE_EGL_SURFACE
#error "Need to bind to the InitFBO framebuffer here."
#endif
  glBindFramebuffer(GL_FRAMEBUFFER, 0 /*default framebuffer*/);

  /* Setup 2D orthographic matrix view
   * scalex, 0,      0,      translatex,
   * 0,      scaley, 0,      translatey,
   * 0,      0,      scalez, translatez,
   * 0,      0,      0,      1
   */
  GLint locOrthoView = glGetUniformLocation(program, "orthoView");
  GLfloat ortho2D[16] = {
      2.0f / appWidth,                 0,    0, 0,
      0,               -2.0f / appHeight,    0, 0,
      0,                               0, 1.0f, 0.0f,
      -1,                           1.0f,    1, 1
  };
  glUniformMatrix4fv(locOrthoView, 1, GL_FALSE, ortho2D);

  GLint locVertexPos = glGetAttribLocation(program, "inVertexPos");
  GLint locVertexCol = glGetAttribLocation(program, "inVertexCol");

#if 0
  GLfloat tex[] = {
    1, 1,
    1, 0,
    0, 0,
    0, 1,
  };

  GLint locTexCoord = glGetAttribLocation(program, "inTexCoord");
  glEnableVertexAttribArray(locTexCoord);
  glVertexAttribPointer(locTexCoord, 2, GL_FLOAT, 0, 0, tex);
#endif

#if 1
  /* pass texture unit to the sampler in the fragment shader */
  GLint locTexId = glGetUniformLocation(program, "texId");
  glUniform1i(locTexId, 0/*GL_TEXTURE0*/);
#endif

  // Generate and Allocate Buffers
  glGenBuffers(1, &vertexPosVBO);
  CheckError();
  glGenBuffers(1, &vertexColVBO);
  CheckError();

  /* buffer allocation */
#if defined(USE_DYNAMIC_STREAMING)
  GLbitfield mapFlags =
    GL_MAP_WRITE_BIT |
    GL_MAP_PERSISTENT_BIT |
    GL_MAP_COHERENT_BIT;
  GLbitfield createFlags = mapFlags | GL_DYNAMIC_STORAGE_BIT;

  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  glBufferStorage(GL_ARRAY_BUFFER, NUM_BUFS * vertexPosSize, NULL, createFlags);
  CheckError();
  glEnableVertexAttribArray(locVertexPos);
  CheckError();
  /* glVertexAttribPointer will always use whatever buffer is currently bound to GL_ARRAY_BUFFER */
  glVertexAttribPointer(locVertexPos, 3/*x,y,z*/, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
  pVertexPosBufferData = (GLfloat *)glMapBufferRange(GL_ARRAY_BUFFER, 0, NUM_BUFS * vertexPosSize, mapFlags);

  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  glBufferStorage(GL_ARRAY_BUFFER, NUM_BUFS * vertexColSize, NULL, createFlags);
  CheckError();
  glEnableVertexAttribArray(locVertexCol);
  CheckError();
  glVertexAttribPointer(locVertexCol, 4/*r,g,b,a*/, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
  pVertexColBufferData = (GLfloat *)glMapBufferRange(GL_ARRAY_BUFFER, 0, NUM_BUFS * vertexColSize, mapFlags);

#else
  pVertexPosBufferData = (GLfloat *)malloc(NUM_BUFS * vertexPosSize);
  assert(pVertexPosBufferData);
  pVertexColBufferData = (GLfloat *)malloc(NUM_BUFS * vertexColSize);
  assert(pVertexColBufferData);

  glBindBuffer(GL_ARRAY_BUFFER, vertexPosVBO);
  CheckError();
  glBufferData(GL_ARRAY_BUFFER, NUM_BUFS * vertexPosSize, NULL, GL_DYNAMIC_DRAW);
  CheckError();
  //printf("GL_MAX_VERTEX_ATTRIBS=%d\n", (int) GL_MAX_VERTEX_ATTRIBS);
  glEnableVertexAttribArray(locVertexPos);
  glVertexAttribPointer(locVertexPos, 3/*x,y,z*/, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
  glBindBuffer(GL_ARRAY_BUFFER, vertexColVBO);
  CheckError();
  glBufferData(GL_ARRAY_BUFFER, NUM_BUFS * vertexColSize, NULL, GL_DYNAMIC_DRAW);
  CheckError();
  glEnableVertexAttribArray(locVertexCol);
  glVertexAttribPointer(locVertexCol, 4/*r,g,b,a*/, GL_FLOAT, GL_FALSE, 0, NULL);
  CheckError();
#endif

          glTexSubImage2D(GL_TEXTURE_2D, 0/*level*/, 0, 0, appWidth, appHeight,
            GL_RGBA, GL_UNSIGNED_BYTE, data);
          CheckError();


  glEnable(GL_DEPTH_TEST);

  /* initialize draw framebuffer to opaque red */
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT /*| GL_DEPTH_BUFFER_BIT*/);
  glFinish();
  eglSwapBuffers(display, surface);
  /* { red is now front buffer } */
  /* clear draw framebuffer to opaque green */
  glClearColor(0, 1, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT /*| GL_DEPTH_BUFFER_BIT*/);
  glFinish();
  eglSwapBuffers(display, surface);
  /* { green is now front buffer, red is now back draw buffer } */

  //tesselateRectangles(Meters);

  /* blit texture (background) into framebuffer */
#if 0
  /* read from background texture, draw into surface */
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbid);
  CheckError();
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  CheckError();
  /* the read and draw framebuffers are those bound to the
   * GL_READ_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER targets respectively. */
  glBlitFramebuffer(
    0, 0, appWidth, appHeight, /*source rectangle */
    0, 0, appWidth, appHeight, /*destination rectangle */
    GL_COLOR_BUFFER_BIT,
    GL_LINEAR/*no scaling anyway*/);
  CheckError();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  CheckError();
#endif

#if 0
  /* draw a full screen rectangle first, copying texture*/
  numRects = 0;
  setColor(0.0f, 0.0f, 0.0f, 0.0f);
  drawRect(0, 0, appWidth/2, appHeight/2);
#endif
  struct timespec ts_action_start, ts_action_end;

  /* total time */
  struct timespec ts_start, ts_end;
  /* frame time; note that frame N-1 end time is frame N start time exactly */
  struct timespec ts_frame_start, ts_frame_end;
  rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
  ts_frame_start = ts_start;

  int frame = 0;
  int num_frames = 1 + 10 * 60;
  int endless = 0;
  int optimize = 0;
  //printf("Rendering %d frames.\n", num_frames);

  // main rendering loop
  while ((frame < num_frames) | endless) {
  printf("frame %d ", frame);

  glClear(GL_DEPTH_BUFFER_BIT);

  /* blit a background image */
#if 0
    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_start);
    /* read from background texture, draw into surface */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbid);
    CheckError();
    // draw into default surface
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    CheckError();
    /* default, blit whole screen */
    int blit_count = 1;
    GLint x = 0;
    GLint y = 0;
    GLint w = appWidth;
    GLint h = appHeight;
#if 0
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

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_end);
    timespec_sub(&ts_action_end, &ts_action_start);
    printf("bg_blit %3.2f ms ", (float)ts_action_end.tv_nsec / 1000000.0f);

#elif 0
    glClear(GL_COLOR_BUFFER_BIT /*| GL_DEPTH_BUFFER_BIT*/);
    CheckError();
#endif

#if 1
    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_start);
    /* update meters */
    updateMeters(Meters, frame);

    /* initialize rectangles */
    clearRectangles(Rect);

    /* sort back to front for transparent objects, using pre-multiplied alpha, but
     * only if ever compositing multiple such objects at the same pixel location. */
    /* sort front to back for opaque objects */

#if 0 /* test depth buffer; increase or decrease depth to see influence on fps */
    for (float depth = 0.5; fabs(depth) < 1.0; depth -= 0.1) {
      /* full screen background */
      addRectangle(Rect, 0, 0, appWidth, appHeight, depth);
    }
#endif

    // wether or not to use optimized meters
    optimize = 0;//(frame > 4);
    if (optimize) {
      addRectanglesFromMetersOptimized(Rect, Meters);
    }else {
      addRectanglesFromMeters(Rect, Meters);
    }

    // blit in partial rectangles 
#if 0
    for (int x = 0; x < appWidth; x += appWidth / 4)
      for (int y = 0; y < appHeight; y += appHeight / 4)
        addRectangle(Rect, x, y, x+appWidth / 8, y+appHeight / 8, 0.5);

    for (int x = 0; x < appWidth; x += appWidth / 8)
      for (int y = 0; y < appHeight; y += appHeight / 8)
        addRectangle(Rect, x+appWidth / 8, y+appHeight / 8, x+appWidth / 8+appWidth / 16, y+appHeight / 8+appHeight /16, 0.6);

    addRectangle(Rect, 0, 0, appWidth, appHeight, +0.9);
#endif

#if 1
    /* read dirty region updates in /tmp/wom0
     * from a fifo */
#if 1
    int fifo_fd = open("/tmp/region_fifo", O_RDONLY | O_NONBLOCK);
    FILE *fifo_stream = NULL;
    if (fifo_fd >= 0) fifo_stream = fdopen(fifo_fd, "r");
#else // fopen does not support non-blocking open  
    FILE *fifo_stream = fopen("/tmp/region_fifo", "r");
#endif
    char *fifo_rc;
    char line_buffer[256];
    if (fifo_stream != NULL)
    do {
      fifo_rc = fgets(&line_buffer[0], 255, fifo_stream);
      //printf("> %s", line_buffer);
      if (fifo_rc == NULL) break;
      if (strcmp(line_buffer, "end of frame\n") == 0) break;
      else {
        int x, y, w, h = 0;
        int scan_rc = sscanf(line_buffer, "region %d %d %d %d", &x, &y, &w, &h);
        if (scan_rc == 4) {
          assert(h == 1);
          //printf("region %d %d %d %d: %d\n", x, y, w, h, scan_rc);
          glTexSubImage2D(GL_TEXTURE_2D, 0/*level*/, x, y, w, h,
            GL_RGBA, GL_UNSIGNED_BYTE, data + (y * w + x) * 4);
          CheckError();
        } else {
          printf("Could not parse region: %s\n", line_buffer);
        }
      }
    } while (fifo_rc != NULL);
    fclose(fifo_stream);
#endif

#if 0
    /* upload (partially dirty region) -- this is one-copy, @TODO we want zero-copy */
    glTexSubImage2D(GL_TEXTURE_2D, 0/*level*/,
      0, 0, appWidth, appHeight,
      GL_RGBA, GL_UNSIGNED_BYTE, data);
    /* GL_RGBA, GL_BGRA */
#endif

    addRectangle(Rect, 0, 0, appWidth, appHeight, +0.9);

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_end);
    timespec_sub(&ts_action_end, &ts_action_start);
    printf("scene %3.2f ms ", (float)ts_action_end.tv_nsec / 1000000.0f);
#endif

#if 1
    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_start);
     /* tesselate rectangles into OpenGL vertex array */
    tesselateRectangles(Rect);
    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_end);
    timespec_sub(&ts_action_end, &ts_action_start);
    printf("tesselate %3.2f ms ", (float)ts_action_end.tv_nsec / 1000000.0f);
#endif
#if 1
    /* flush buffers and commit drawing instructions to GPU */
    flushAndCommit();
#endif

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_start);
#if 0
    glFinish();
#endif
    assert(surface != EGL_NO_SURFACE);
    if (surface == EGL_NO_SURFACE) {
      /* glFlush() ensures all commands are on the GPU */
      /* glFinish() ensures all commands are also finished */
      glFinish();
    } else {

      /* prove that we have X ms of CPU time left per iteration, X ~= 16+ */
      /* increase this number until the FPS is affected */
      //usleep(16*1000);

      /* @TODO what is the guaranteed behaviour regarding back and front
       * buffer content copies and damage. See eglSwapBuffersWithDamageKHR():
       * https://www.khronos.org/registry/EGL/extensions/KHR/EGL_KHR_swap_buffers_with_damage.txt
       */
      eglSwapBuffers(display, surface);
      //struct gbm_bo_tiling tiling;
      struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
      assert(bo);
      if (bo) {
        /* prove that multi-buffering is used */
        EGLint handle = gbm_bo_get_handle(bo).u32;
        //printf("frame %d handle %d\n", frame, (int)handle);
        /* @TODO typically pass these along with DMA BUF */
        uint32_t width = gbm_bo_get_width(bo);
        uint32_t height = gbm_bo_get_height(bo);
        uint32_t stride = gbm_bo_get_stride(bo);
        int planes = gbm_bo_get_plane_count(bo);
        uint64_t modifier = gbm_bo_get_modifier(bo);
#if 0
        printf("%u x %u, stride %u bytes, %d planes\n", width, height, stride, planes);
        if (modifier == I915_FORMAT_MOD_X_TILED) {
          printf("I915_FORMAT_MOD_X_TILED\n");
        }
#endif
        /* gbm_bo_get_fd() will create a DMA-BUF and return its file descriptor */
        int fd = gbm_bo_get_fd(bo);
        assert(fd >= 0);
        if (fd >= 0) {
          /* open FPGA DMA */
          int sgdma_fd = open("/dev/xdma0_h2c_0", O_RDWR);
          //assert(sgdma_fd >= 0);
          if (sgdma_fd >= 0) {
            rc = ioctl(sgdma_fd, IOCTL_XDMA_IMPORT_DMABUF, &fd);
            assert(rc >= 0);
            close(sgdma_fd);
          } else {

          }
          close(fd);
        }
      }
      if (previous_bo) {
        gbm_surface_release_buffer(gs, previous_bo);
      }
      previous_bo = bo;
    }

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_action_end);
    timespec_sub(&ts_action_end, &ts_action_start);
    printf("swap %3.2f ms ", (float)ts_action_end.tv_nsec / 1000000.0f);

    rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts_frame_end);
    struct timespec ts_frame_start_next = ts_frame_end;
    /* subtract the start time from the end time */
    timespec_sub(&ts_frame_end, &ts_frame_start);
    ts_frame_start = ts_frame_start_next;
    if (ts_frame_end.tv_sec > 0) {
      printf("frame took longer than 1 second?\n");
    }
#if 0
    if (!(frame % 5)) {
#else
    if (1) {
#endif
      printf("total %3.2f ms (fps = %3.2f)\n",
      (float)ts_frame_end.tv_nsec / 1000000.0f, 1000000000.0f / (float)ts_frame_end.tv_nsec);
    }

    /* generate a PNG every 60 frames */
    if ((frame > 0) && (frame % 60) == 0) {
      GLubyte *content;
      content = malloc(appWidth * appHeight * 4);
      assert(content);
      //printf("glFinish()\n");
      glFinish();
      //printf("glReadPixels()\n");
      // default is GL_BACK for double buffering
      glReadBuffer(GL_BACK);
      glReadPixels(0, 0, appWidth, appHeight, GL_RGBA, GL_UNSIGNED_BYTE, content);
      CheckError();
      printf("writeImage() frame %d\n");
      char png_output_filename[256];
      snprintf(&png_output_filename[0], 255, "frame%d.png", frame);
      assert(!writeImage(png_output_filename, appWidth, appHeight, content, "gbm-egl-compositing"));
      free(content);
    }

    optimize = 1;
    frame++;
  }

  rc = clock_gettime(CLOCK_MONOTONIC, &ts_end);
  /* subtract the start time from the end time */
  timespec_sub(&ts_end, &ts_start);
  printf("CLOCK_MONOTONIC reports %ld.%09ld seconds\n",
    ts_end.tv_sec, ts_end.tv_nsec);

  if (previous_bo) {
    gbm_surface_release_buffer(gs, previous_bo);
  }

#ifndef USE_DYNAMIC_STREAMING
  free(pVertexPosBufferData);
  free(pVertexColBufferData);
#endif

  glDeleteBuffers(1, &vertexPosVBO);
  glDeleteBuffers(1, &vertexColVBO);

  free(Meters); Meters = NULL;
}

int inspect_gl(void) {
  int gl_version = epoxy_gl_version();
  printf("epoxy_gl_version() = %d\n", gl_version);
  bool is_desktop_gl = epoxy_is_desktop_gl();
  printf("epoxy_is_desktop_gl() = %d\n", (int)is_desktop_gl);
  bool has_intel = epoxy_has_gl_extension("GL_INTEL_map_texture");
  printf("epoxy_has_gl_extension(GL_INTEL_map_texture) = %d\n", (int)has_intel);

  int num_extensions;
  glGetIntegerv(GL_NUM_EXTENSIONS, &num_extensions);
  printf("GL_NUM_EXTENSIONS = %d\n", num_extensions);

  for (int i = 0; i < num_extensions; i++) {
    char *ext = (char *)glGetStringi(GL_EXTENSIONS, i);
    if (ext) {
      printf("%s\n", ext);
    }
  }
}

int main(void)
{
  RenderTargetInit();
  inspect_gl();
  InitGLES();
  Render();
  return 0;
}
