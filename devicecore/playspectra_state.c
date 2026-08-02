// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

/*!
 * @file
 * @brief  Shared refcounted VirtualDeviceState (see playspectra_state.h).
 */

#include "playspectra_state.h"

#include "ps_os.h"

#include <stdbool.h>
#include <stdlib.h>

#define PS_HAPTIC_QUEUE 16

struct playspectra_state
{
	struct ps_mutex mutex; // guards everything below
	int refcount;

	struct playspectra_pose head;
	struct playspectra_ctrl ctrl[2]; // [PLAYSPECTRA_LEFT], [PLAYSPECTRA_RIGHT]

	// reset(spec §5.3)用の起動時スナップショット。Adapter が全デバイスの初期 pose を
	// 書き込んだ後に capture_initial で採取し、reset で head/ctrl をここへ戻す。
	struct playspectra_pose initial_head;
	struct playspectra_ctrl initial_ctrl[2];
	bool has_initial;

	// haptic イベントのリング(アプリ set_output → 制御チャネルが転送)。
	struct playspectra_haptic_event haptics[PS_HAPTIC_QUEUE];
	int haptic_head;
	int haptic_count;

	struct playspectra_control *control; // taken exactly once at teardown
	bool control_taken;
};

struct playspectra_state *
playspectra_state_create(void)
{
	struct playspectra_state *s = calloc(1, sizeof(struct playspectra_state));
	if (s == NULL) {
		return NULL;
	}
	ps_mutex_init(&s->mutex);
	s->refcount = 1;
	// head は all-zero(playspectra_pose のフラグ全 false = pose 無効)。旧実装の
	// XRT_SPACE_RELATION_ZERO(flags=0)と同じ意味。calloc 済みなので明示代入は不要。
	return s;
}

void
playspectra_state_ref(struct playspectra_state *s)
{
	ps_mutex_lock(&s->mutex);
	s->refcount++;
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_unref(struct playspectra_state *s)
{
	if (s == NULL) {
		return;
	}
	ps_mutex_lock(&s->mutex);
	int rc = --s->refcount;
	ps_mutex_unlock(&s->mutex);
	if (rc == 0) {
		ps_mutex_destroy(&s->mutex);
		free(s);
	}
}

void
playspectra_state_set_head(struct playspectra_state *s, const struct playspectra_pose *pose)
{
	ps_mutex_lock(&s->mutex);
	s->head = *pose;
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_get_head(struct playspectra_state *s, struct playspectra_pose *out)
{
	ps_mutex_lock(&s->mutex);
	*out = s->head;
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_set_ctrl(struct playspectra_state *s, enum playspectra_hand hand, const struct playspectra_ctrl *c)
{
	ps_mutex_lock(&s->mutex);
	s->ctrl[hand] = *c;
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_get_ctrl(struct playspectra_state *s, enum playspectra_hand hand, struct playspectra_ctrl *out)
{
	ps_mutex_lock(&s->mutex);
	*out = s->ctrl[hand];
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_capture_initial(struct playspectra_state *s)
{
	ps_mutex_lock(&s->mutex);
	s->initial_head = s->head;
	s->initial_ctrl[PLAYSPECTRA_LEFT] = s->ctrl[PLAYSPECTRA_LEFT];
	s->initial_ctrl[PLAYSPECTRA_RIGHT] = s->ctrl[PLAYSPECTRA_RIGHT];
	s->has_initial = true;
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_reset(struct playspectra_state *s)
{
	ps_mutex_lock(&s->mutex);
	if (s->has_initial) {
		s->head = s->initial_head;
		s->ctrl[PLAYSPECTRA_LEFT] = s->initial_ctrl[PLAYSPECTRA_LEFT];
		s->ctrl[PLAYSPECTRA_RIGHT] = s->initial_ctrl[PLAYSPECTRA_RIGHT];
	}
	ps_mutex_unlock(&s->mutex);
}

void
playspectra_state_push_haptic(struct playspectra_state *s, const struct playspectra_haptic_event *e)
{
	ps_mutex_lock(&s->mutex);
	if (s->haptic_count == PS_HAPTIC_QUEUE) {
		// 満杯: 最古を捨てる。
		s->haptic_head = (s->haptic_head + 1) % PS_HAPTIC_QUEUE;
		s->haptic_count--;
	}
	int idx = (s->haptic_head + s->haptic_count) % PS_HAPTIC_QUEUE;
	s->haptics[idx] = *e;
	s->haptic_count++;
	ps_mutex_unlock(&s->mutex);
}

bool
playspectra_state_pop_haptic(struct playspectra_state *s, struct playspectra_haptic_event *out)
{
	ps_mutex_lock(&s->mutex);
	bool has = s->haptic_count > 0;
	if (has) {
		*out = s->haptics[s->haptic_head];
		s->haptic_head = (s->haptic_head + 1) % PS_HAPTIC_QUEUE;
		s->haptic_count--;
	}
	ps_mutex_unlock(&s->mutex);
	return has;
}

void
playspectra_state_set_control(struct playspectra_state *s, struct playspectra_control *c)
{
	ps_mutex_lock(&s->mutex);
	s->control = c;
	ps_mutex_unlock(&s->mutex);
}

struct playspectra_control *
playspectra_state_take_control(struct playspectra_state *s)
{
	ps_mutex_lock(&s->mutex);
	struct playspectra_control *c = NULL;
	if (!s->control_taken) {
		s->control_taken = true;
		c = s->control;
		s->control = NULL;
	}
	ps_mutex_unlock(&s->mutex);
	return c;
}
