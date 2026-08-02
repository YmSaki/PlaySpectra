// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

/*!
 * @file
 * @brief  Minimal OS abstraction (mutex + control thread) for the PlaySpectra
 *         device core. Monado の os_threading.h 相当の最小サブセットで、Core を
 *         ランタイム非依存にするための置き換え。セマンティクスは os_thread_helper と
 *         同一: start で running=true + スレッド生成、is_running はロック下で読み、
 *         stop_and_wait は running=false にして join する。
 */

#pragma once

#include <stdbool.h>

#ifdef _WIN32
// windows.h は既定で winsock.h(旧)を引き込み、後段の winsock2.h/ws2tcpip.h と衝突して
// 構文エラーの嵐になる(MSVC 実測)。LEAN_AND_MEAN で winsock.h を除外する。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Plain mutex (Win32 CRITICAL_SECTION / pthread_mutex_t).
 */
struct ps_mutex
{
#ifdef _WIN32
	CRITICAL_SECTION cs;
#else
	pthread_mutex_t mtx;
#endif
};

void
ps_mutex_init(struct ps_mutex *m);

void
ps_mutex_lock(struct ps_mutex *m);

void
ps_mutex_unlock(struct ps_mutex *m);

void
ps_mutex_destroy(struct ps_mutex *m);

/*!
 * Worker-thread helper with a mutex-guarded running flag
 * (os_thread_helper と同じ停止プロトコル: フラグを下ろしてから join)。
 */
struct ps_thread
{
	struct ps_mutex lock;
	bool running;
	bool started;
#ifdef _WIN32
	HANDLE handle;
#else
	pthread_t handle;
#endif
	void *(*func)(void *);
	void *arg;
};

void
ps_thread_init(struct ps_thread *t);

//! Returns 0 on success (os_thread_helper_start と同じ規約)。
int
ps_thread_start(struct ps_thread *t, void *(*func)(void *), void *arg);

bool
ps_thread_is_running(struct ps_thread *t);

//! running フラグを下ろしてスレッドを join する(未 start なら何もしない)。
void
ps_thread_stop_and_wait(struct ps_thread *t);

void
ps_thread_destroy(struct ps_thread *t);

#ifdef __cplusplus
}
#endif
