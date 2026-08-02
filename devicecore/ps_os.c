// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

/*!
 * @file
 * @brief  Minimal OS abstraction implementation (see ps_os.h).
 */

#include "ps_os.h"

#ifdef _WIN32
#include <process.h> // _beginthreadex
#endif

void
ps_mutex_init(struct ps_mutex *m)
{
#ifdef _WIN32
	InitializeCriticalSection(&m->cs);
#else
	pthread_mutex_init(&m->mtx, NULL);
#endif
}

void
ps_mutex_lock(struct ps_mutex *m)
{
#ifdef _WIN32
	EnterCriticalSection(&m->cs);
#else
	pthread_mutex_lock(&m->mtx);
#endif
}

void
ps_mutex_unlock(struct ps_mutex *m)
{
#ifdef _WIN32
	LeaveCriticalSection(&m->cs);
#else
	pthread_mutex_unlock(&m->mtx);
#endif
}

void
ps_mutex_destroy(struct ps_mutex *m)
{
#ifdef _WIN32
	DeleteCriticalSection(&m->cs);
#else
	pthread_mutex_destroy(&m->mtx);
#endif
}

#ifdef _WIN32
// _beginthreadex は unsigned __stdcall を要求するため、void *(*)(void *) を包む。
static unsigned __stdcall
ps_thread_trampoline(void *ptr)
{
	struct ps_thread *t = (struct ps_thread *)ptr;
	t->func(t->arg);
	return 0;
}
#else
static void *
ps_thread_trampoline(void *ptr)
{
	struct ps_thread *t = (struct ps_thread *)ptr;
	return t->func(t->arg);
}
#endif

void
ps_thread_init(struct ps_thread *t)
{
	ps_mutex_init(&t->lock);
	t->running = false;
	t->started = false;
	t->func = NULL;
	t->arg = NULL;
}

int
ps_thread_start(struct ps_thread *t, void *(*func)(void *), void *arg)
{
	t->func = func;
	t->arg = arg;
	// スレッド本体が起動直後に is_running を見るため、生成前に立てる(os_thread_helper と同じ)。
	ps_mutex_lock(&t->lock);
	t->running = true;
	ps_mutex_unlock(&t->lock);
#ifdef _WIN32
	t->handle = (HANDLE)_beginthreadex(NULL, 0, ps_thread_trampoline, t, 0, NULL);
	if (t->handle == NULL) {
		ps_mutex_lock(&t->lock);
		t->running = false;
		ps_mutex_unlock(&t->lock);
		return -1;
	}
#else
	if (pthread_create(&t->handle, NULL, ps_thread_trampoline, t) != 0) {
		ps_mutex_lock(&t->lock);
		t->running = false;
		ps_mutex_unlock(&t->lock);
		return -1;
	}
#endif
	t->started = true;
	return 0;
}

bool
ps_thread_is_running(struct ps_thread *t)
{
	ps_mutex_lock(&t->lock);
	bool r = t->running;
	ps_mutex_unlock(&t->lock);
	return r;
}

void
ps_thread_stop_and_wait(struct ps_thread *t)
{
	ps_mutex_lock(&t->lock);
	t->running = false;
	ps_mutex_unlock(&t->lock);
	if (!t->started) {
		return;
	}
#ifdef _WIN32
	WaitForSingleObject(t->handle, INFINITE);
	CloseHandle(t->handle);
#else
	pthread_join(t->handle, NULL);
#endif
	t->started = false;
}

void
ps_thread_destroy(struct ps_thread *t)
{
	ps_mutex_destroy(&t->lock);
}
