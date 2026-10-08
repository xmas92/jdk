/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <atomic>
#include <thread>

#include "jni.h"

static std::atomic<int> state{0};
static jint active_scenario;
static jobject nested_handle;
static jfieldID nested_field;

extern "C" JNIEXPORT jboolean JNICALL
Java_TestJNIFastGetFieldAfterGC_ready(JNIEnv* env, jclass klass) {
  return state.load(std::memory_order_acquire) == 1;
}

extern "C" JNIEXPORT void JNICALL
Java_TestJNIFastGetFieldAfterGC_resume(JNIEnv* env, jclass klass) {
  state.store(2, std::memory_order_release);
}

static void await_gc() {
  // Await GC
  state.store(1, std::memory_order_release);
  while (state.load(std::memory_order_acquire) != 2) {
    std::this_thread::yield();
  }
}

static jlong call_callback(JNIEnv* env, jclass klass) {
  const jmethodID callback = env->GetStaticMethodID(klass, "nestedCallback", "()J");
  return env->CallStaticLongMethod(klass, callback);
}

extern "C" JNIEXPORT jlong JNICALL
Java_TestJNIFastGetFieldAfterGC_readFromCallback(JNIEnv* env, jclass klass) {
  await_gc();
  const bool read_in_callback = active_scenario >= 9;
  if (read_in_callback) {
    return env->GetLongField(nested_handle, nested_field);
  }
  return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_TestJNIFastGetFieldAfterGC_readInNative(JNIEnv* env, jclass klass, jobject item, jint scenario) {
  jobject handle;

  // 1-4:  Wait and read directly: parameter, local, global, weak global.
  // 5-8:  Wait in callback and read directly: same reference types.
  // 9-10: Wait and read in callback: global, weak global.

  // Create handle
  switch (scenario) {
    case 1:
    case 5: handle = item; break;
    case 2:
    case 6: handle = env->NewLocalRef(item); break;
    case 3:
    case 7:
    case 9: handle = env->NewGlobalRef(item); break;
    case 4:
    case 8:
    case 10: handle = env->NewWeakGlobalRef(item); break;
    // Unexpected scenario
    default: return -1;
  }

  // Resolve the field before the GC
  const jclass item_class = env->GetObjectClass(item);
  const jfieldID field = env->GetFieldID(item_class, "value", "J");
  env->DeleteLocalRef(item_class);

  // Execute scenario
  const bool await_in_callback = scenario >= 5;
  const bool read_in_callback = scenario >= 9;
  jlong result;
  if (await_in_callback) {
    active_scenario = scenario;
    if (read_in_callback) {
      nested_handle = handle;
      nested_field = field;
    }
    result = call_callback(env, klass);
    if (!read_in_callback) {
      result = env->GetLongField(handle, field);
    }
    nested_handle = nullptr;
    nested_field = nullptr;
    active_scenario = 0;
  } else {
    await_gc();
    result = env->GetLongField(handle, field);
  }

  // Clean up handle
  switch (scenario) {
    case 2:
    case 6: env->DeleteLocalRef(handle); break;
    case 3:
    case 7:
    case 9: env->DeleteGlobalRef(handle); break;
    case 4:
    case 8:
    case 10: env->DeleteWeakGlobalRef((jweak)handle); break;
  }

  // Reset state
  state.store(0, std::memory_order_release);

  return result;
}
