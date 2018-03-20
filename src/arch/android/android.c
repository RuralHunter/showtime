/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <limits.h>
#include <syslog.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <signal.h>
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <GLES2/gl2.h>
#include <sys/mman.h>

#include "arch/arch.h"
#include "main.h"
#include "service.h"
#include "networking/net.h"
#include "ui/glw/glw.h"
#include "prop/prop_jni.h"
#include "android.h"
#include "navigator.h"
#include "arch/halloc.h"
#include "misc/md5.h"
#include "misc/str.h"
#include "text/text.h"

static char android_manufacturer[PROP_VALUE_MAX];
static char android_model[PROP_VALUE_MAX];
static char android_name[PROP_VALUE_MAX];
static char android_version[PROP_VALUE_MAX];
static char android_serialno[PROP_VALUE_MAX];

JavaVM *JVM;
jclass STCore;
prop_t *android_nav;

/**
 *
 */
void
trace_arch(int level, const char *prefix, const char *str)
{
  int prio;
  switch(level) {
  case TRACE_EMERG:   prio = ANDROID_LOG_FATAL; break;
  case TRACE_ERROR:   prio = ANDROID_LOG_ERROR; break;
  case TRACE_INFO:    prio = ANDROID_LOG_INFO;  break;
  case TRACE_DEBUG:   prio = ANDROID_LOG_DEBUG; break;
  default:            prio = ANDROID_LOG_ERROR; break;
  }
  __android_log_print(prio, APPNAMEUSER, "%s %s", prefix, str);
}


/**
 *
 */
int64_t
arch_get_ts(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/**
 *
 */
int64_t
arch_cache_avail_bytes(void)
{
  struct statfs buf;

  if(gconf.cache_path == NULL || statfs(gconf.cache_path, &buf))
    return 0;

  return buf.f_bfree * buf.f_bsize;
}


/**
 *
 */
size_t
arch_malloc_size(void *ptr)
{
  return 0;
}


/**
 *
 */
void *
halloc(size_t size)
{
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(p == MAP_FAILED)
    return NULL;
  return p;
}

/**
 *
 */
void
hfree(void *ptr, size_t size)
{
  munmap(ptr, size);
}


void
my_localtime(const time_t *now, struct tm *tm)
{
  localtime_r(now, tm);
}


int
arch_pipe(int pipefd[2])
{
  return pipe(pipefd);
}

void *
mymalloc(size_t size)
{
  return malloc(size);
}

void *
myrealloc(void *ptr, size_t size)
{
  return realloc(ptr, size);
}

void *
mycalloc(size_t count, size_t size)
{
  return calloc(count, size);
}

void *
mymemalign(size_t align, size_t size)
{
  return memalign(align, size);
}

const char *
arch_get_system_type(void)
{
  return "Android";
}

void
arch_exit(void)
{
  exit(0);
}

int
arch_stop_req(void)
{
  return ARCH_STOP_CALLER_MUST_HANDLE;
}

void
arch_localtime(const time_t *now, struct tm *tm)
{
  localtime_r(now, tm);
}

static int devurandom;

void
arch_get_random_bytes(void *ptr, size_t size)
{
  ssize_t r = read(devurandom, ptr, size);
  if(r != size)
    abort();
}

INITIALIZER(opendevurandom) {
  devurandom = open("/dev/urandom", O_RDONLY);
};


/**
 *
 */
int64_t
arch_get_avtime(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * 1000000000LL + ts.tv_nsec) / 1000LL;
}


jint
JNI_OnLoad(JavaVM *vm, void *reserved)
{
  JVM = vm;

  __system_property_get("ro.product.manufacturer",  android_manufacturer);
  __system_property_get("ro.product.model",         android_model);
  __system_property_get("ro.product.name",          android_name);
  __system_property_get("ro.build.version.release", android_version);
  __system_property_get("ro.serialno",              android_serialno);

  snprintf(gconf.os_info, sizeof(gconf.os_info), "%s", android_version);

  snprintf(gconf.device_type, sizeof(gconf.device_type),
           "%s %s", android_manufacturer, android_model);

  return JNI_VERSION_1_6;
}

/**
 *
 */
static void
preload_fonts(void)
{
  freetype_load_default_font("bundle:///res/fonts/MicroHei.ttf", 100);
}


/**
 *
 */
JNIEXPORT void JNICALL
Java_com_lonelycoder_mediaplayer_Core_coreInit(JNIEnv *env, jobject obj, jstring j_settings, jstring j_cachedir, jstring j_sdcard, jstring j_android_id, jint time_24hrs, jstring j_music, jstring j_pictures, jstring j_movies, jint audio_sample_rate, jint audio_frames_per_buffer)
{
  char path[PATH_MAX];
  trace_arch(TRACE_INFO, "Core", "Native core initializing");

  gconf.trace_level = TRACE_DEBUG;
  gconf.time_format_system = time_24hrs ? TIME_FORMAT_24 : TIME_FORMAT_12;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(tv.tv_usec);

  const char *settings   = (*env)->GetStringUTFChars(env, j_settings, 0);
  const char *cachedir   = (*env)->GetStringUTFChars(env, j_cachedir, 0);
  const char *sdcard     = (*env)->GetStringUTFChars(env, j_sdcard, 0);
  const char *android_id = (*env)->GetStringUTFChars(env, j_android_id, 0);

  gconf.persistent_path = strdup(settings);
  gconf.cache_path      = strdup(cachedir);

  snprintf(path, sizeof(path), "%s/Download", sdcard);
  mkdir(path, 0770);
  snprintf(path, sizeof(path), "%s/Download/movian_upgrade.apk", sdcard);
  gconf.upgrade_path = strdup(path);
  unlink(gconf.upgrade_path);

  uint8_t digest[16];

  md5_decl(ctx);
  md5_init(ctx);

  md5_update(ctx, (const void *)android_id, strlen(android_id));
  md5_update(ctx, (const void *)android_serialno, strlen(android_serialno));

  md5_final(ctx, digest);
  bin2hex(gconf.device_id, sizeof(gconf.device_id), digest, sizeof(digest));


  (*env)->ReleaseStringUTFChars(env, j_settings, settings);
  (*env)->ReleaseStringUTFChars(env, j_cachedir, cachedir);
  (*env)->ReleaseStringUTFChars(env, j_sdcard,   sdcard);
  (*env)->ReleaseStringUTFChars(env, j_android_id, android_id);


  gconf.concurrency =   sysconf(_SC_NPROCESSORS_CONF);

  setlocale(LC_ALL, "");

  signal(SIGPIPE, SIG_IGN);

  main_init();

  jclass c = (*env)->FindClass(env, "com/lonelycoder/mediaplayer/Core");
  STCore = (*env)->NewGlobalRef(env, c);

  prop_jni_init(env);

  service_createp("androidstorage", _p("Android Storage"),
                  "file:///storage",
                  "storage", NULL, 0, 1, SVC_ORIGIN_SYSTEM);
  preload_fonts();
  
  android_nav = nav_spawn();

  extern int android_system_audio_sample_rate;
  extern int android_system_audio_frames_per_buffer;

  android_system_audio_sample_rate = audio_sample_rate;
  android_system_audio_frames_per_buffer = audio_frames_per_buffer;
}


/**
 *
 */
JNIEXPORT void JNICALL
Java_com_lonelycoder_mediaplayer_Core_pollCourier(JNIEnv *env, jobject obj)
{
  prop_jni_poll();
}


/**
 *
 */
JNIEXPORT void JNICALL
Java_com_lonelycoder_mediaplayer_Core_networkStatusChanged(JNIEnv *env, jobject obj)
{
  extern void asyncio_trig_network_change(void);
  asyncio_trig_network_change();
}
