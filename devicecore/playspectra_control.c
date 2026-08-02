// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

/*!
 * @file
 * @brief  PlaySpectra NDJSON control channel (spec playspectra-device-core-spec.md §5).
 *
 * 複数接続を単一スレッドの select() 多重化で捌く(spec §5.3)。hello の role で
 * writer(同時1接続のみ排他) / observer(複数可) を割り当てる。set_state は writer のみ、
 * get_state / status は誰でも、haptic イベントは接続中の全ハンドシェイク済みクライアントへ
 * broadcast する。set_state の hmd.head と left/right コントローラを共有 VirtualDeviceState
 * (playspectra_state)へ書き込み、get_state はそこから読み出す(各デバイスも共有 state から読む)。
 * socket のクロスプラットフォームラップは Monado の remote ドライバ(r_hub.c)と同型。
 *
 * blocking accept()/recv() は Linux では close() で確実に解除されない(Monado/WSL で実測)ため、
 * select() に 200ms タイムアウトを付け、stop フラグと haptic キューを定期的にチェックできるようにする。
 *
 * ランタイム固有の値(ポート・runtime 名・descriptor・ログ)は playspectra_control_config で
 * Adapter 殻から注入される(playspectra_control.h)。core は環境変数もランタイム API も読まない。
 */

#include "playspectra_control.h"
#include "playspectra_proto.h"
#include "playspectra_state.h"
#include "ps_os.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ps_socket_t;
#define PS_INVALID_SOCKET INVALID_SOCKET
#define ps_close_socket closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int ps_socket_t;
#define PS_INVALID_SOCKET (-1)
#define ps_close_socket close
#endif

#define PS_PROTOCOL_VERSION PLAYSPECTRA_PROTOCOL_VERSION
#define PS_LINE_MAX (1024 * 1024) // spec §5.1: 最大 1 MiB
#define PS_MAX_CONN 8             // 1 writer + 最大 7 observer(FD_SETSIZE 十分内)
#define PS_RUNTIME_NAME_MAX 32

// 1接続あたりのロール(spec §5.3)。hello 完了までは NONE。
enum ps_role
{
	PS_ROLE_NONE = 0, // hello 未完了(イベント購読対象外)
	PS_ROLE_WRITER,   // 書き込み権あり(同時1接続のみ)
	PS_ROLE_OBSERVER, // 読み取り/購読のみ(複数可)
};

// 1接続の状態。sock == PS_INVALID_SOCKET が空きスロット。buf は行組み立て用(スロット再利用のため保持)。
struct ps_conn
{
	ps_socket_t sock;
	enum ps_role role;
	char *buf;   // PS_LINE_MAX(遅延確保・stop で一括 free)
	size_t used; // buf に溜めた行の長さ
};

struct playspectra_control
{
	struct playspectra_state *state; // 共有 VirtualDeviceState(ref を1つ保持)
	struct ps_thread oth;
	ps_socket_t listen_sock;
	uint16_t port;
	uint64_t sequence; // 最後に適用した sequence(realtime latest-wins)

	// Adapter 殻から注入された設定(playspectra_control_config のコピー)。
	char runtime_name[PS_RUNTIME_NAME_MAX];
	int recommended_eye_width;
	int recommended_eye_height;
	int refresh_hz;
	void (*log_fn)(void *user, enum ps_log_level level, const char *msg);
	void *log_user;

	// frame_synchronized(spec §4): 直近に適用した logical_frame とその内容署名。
	bool has_frame;
	int64_t last_frame;
	uint64_t frame_sig;

	struct ps_conn conns[PS_MAX_CONN];
};

// 整形して config.log_fn へ渡す(NULL なら無音)。レベルゲートは殻の責務。
static void
ctl_log(struct playspectra_control *c, enum ps_log_level level, const char *fmt, ...)
{
	if (c->log_fn == NULL) {
		return;
	}
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	c->log_fn(c->log_user, level, msg);
}


/*
 * 接続スロット / ロール ヘルパ。
 */

// この接続以外に writer ロールの接続が存在するか(writer 排他判定)。
static bool
writer_held_by_other(struct playspectra_control *c, const struct ps_conn *self)
{
	for (int i = 0; i < PS_MAX_CONN; i++) {
		const struct ps_conn *cn = &c->conns[i];
		if (cn != self && cn->sock != PS_INVALID_SOCKET && cn->role == PS_ROLE_WRITER) {
			return true;
		}
	}
	return false;
}

// writer ロールの接続が1つでもあるか(status 用)。
static bool
any_writer(struct playspectra_control *c)
{
	return writer_held_by_other(c, NULL);
}

// 空きスロットへ新規接続を登録。満杯 or buf 確保失敗なら NULL。
static struct ps_conn *
alloc_conn(struct playspectra_control *c, ps_socket_t s)
{
	for (int i = 0; i < PS_MAX_CONN; i++) {
		struct ps_conn *cn = &c->conns[i];
		if (cn->sock != PS_INVALID_SOCKET) {
			continue;
		}
		if (cn->buf == NULL) {
			cn->buf = malloc(PS_LINE_MAX);
			if (cn->buf == NULL) {
				return NULL;
			}
		}
		cn->sock = s;
		cn->role = PS_ROLE_NONE;
		cn->used = 0;
		return cn;
	}
	return NULL; // 満杯
}

// 接続を閉じてスロットを空ける(buf は再利用のため保持、stop で free)。
static void
free_conn(struct ps_conn *cn)
{
	if (cn->sock != PS_INVALID_SOCKET) {
		ps_close_socket(cn->sock);
		cn->sock = PS_INVALID_SOCKET;
	}
	cn->role = PS_ROLE_NONE;
	cn->used = 0;
}


/*
 * JSON send helpers (パースは playspectra_proto.c に分離)。
 */

// 1行を送る(末尾に \n)。cJSON は呼び出し側で delete する。
static void
send_json_line(ps_socket_t s, cJSON *obj)
{
	char *str = cJSON_PrintUnformatted(obj);
	if (str == NULL) {
		return;
	}
	size_t len = strlen(str);
	(void)send(s, str, (int)len, 0);
	(void)send(s, "\n", 1, 0);
	cJSON_free(str);
}

static void
reply_error(ps_socket_t s, const cJSON *req, const char *error_type, const char *error)
{
	cJSON *r = cJSON_CreateObject();
	const cJSON *rid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
	if (cJSON_IsString(rid)) {
		cJSON_AddStringToObject(r, "request_id", rid->valuestring);
	}
	cJSON_AddBoolToObject(r, "ok", false);
	cJSON_AddStringToObject(r, "error_type", error_type);
	cJSON_AddStringToObject(r, "error", error);
	send_json_line(s, r);
	cJSON_Delete(r);
}

static void
reply_ok(ps_socket_t s, const cJSON *req, cJSON *extra /*owned*/)
{
	cJSON *r = extra != NULL ? extra : cJSON_CreateObject();
	const cJSON *rid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
	if (cJSON_IsString(rid)) {
		cJSON_AddStringToObject(r, "request_id", rid->valuestring);
	}
	cJSON_AddBoolToObject(r, "ok", true);
	send_json_line(s, r);
	cJSON_Delete(r);
}


/*
 * pose <-> JSON 変換。state はランタイム中立の playspectra_pose を保持するので変換は不要。
 */

// playspectra_pose → PoseState JSON(spec §2.1)。velocity は未追跡なので null(spec §7)。
static cJSON *
pose_to_json(const struct playspectra_pose *pose)
{
	cJSON *p = cJSON_CreateObject();
	cJSON *pos = cJSON_AddArrayToObject(p, "position");
	cJSON_AddItemToArray(pos, cJSON_CreateNumber(pose->position[0]));
	cJSON_AddItemToArray(pos, cJSON_CreateNumber(pose->position[1]));
	cJSON_AddItemToArray(pos, cJSON_CreateNumber(pose->position[2]));
	cJSON *ori = cJSON_AddArrayToObject(p, "orientation");
	cJSON_AddItemToArray(ori, cJSON_CreateNumber(pose->orientation[0]));
	cJSON_AddItemToArray(ori, cJSON_CreateNumber(pose->orientation[1]));
	cJSON_AddItemToArray(ori, cJSON_CreateNumber(pose->orientation[2]));
	cJSON_AddItemToArray(ori, cJSON_CreateNumber(pose->orientation[3]));
	cJSON_AddNullToObject(p, "linear_velocity");  // 未追跡: null 許容(spec §2.1/§7)
	cJSON_AddNullToObject(p, "angular_velocity"); // 同上
	cJSON *fl = cJSON_AddObjectToObject(p, "relation_flags");
	cJSON_AddBoolToObject(fl, "position_valid", pose->position_valid);
	cJSON_AddBoolToObject(fl, "orientation_valid", pose->orientation_valid);
	cJSON_AddBoolToObject(fl, "position_tracked", pose->position_tracked);
	cJSON_AddBoolToObject(fl, "orientation_tracked", pose->orientation_tracked);
	return p;
}

// 共有 state のコントローラ → Controller JSON(spec §2.3)。inputs は apply_ctrl が受理する
// semantic path と対称に出す(左=x/y+menu, 右=a/b+system)。system の runtime_reserved 分離
// (spec §2.4)と Descriptor 駆動の完全性は M2 後続で確定(§7)。
static cJSON *
ctrl_to_json(struct playspectra_control *c, enum playspectra_hand hand)
{
	struct playspectra_ctrl st;
	playspectra_state_get_ctrl(c->state, hand, &st);

	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "connected", st.connected);
	if (!st.connected) {
		return o; // 未接続は他フィールド不要(spec §2.2)
	}
	cJSON_AddItemToObject(o, "grip", pose_to_json(&st.grip));
	cJSON_AddItemToObject(o, "aim", pose_to_json(&st.aim));

	const bool is_left = (hand == PLAYSPECTRA_LEFT);
	cJSON *in = cJSON_AddObjectToObject(o, "inputs");
	cJSON_AddNumberToObject(in, "/input/trigger/value", st.trigger);
	cJSON_AddBoolToObject(in, "/input/trigger/touch", st.trigger_touch);
	cJSON_AddNumberToObject(in, "/input/squeeze/value", st.squeeze);
	cJSON_AddNumberToObject(in, "/input/thumbstick/x", st.thumbstick_x);
	cJSON_AddNumberToObject(in, "/input/thumbstick/y", st.thumbstick_y);
	cJSON_AddBoolToObject(in, "/input/thumbstick/click", st.thumbstick_click);
	cJSON_AddBoolToObject(in, "/input/thumbstick/touch", st.thumbstick_touch);
	cJSON_AddBoolToObject(in, "/input/thumbrest/touch", st.thumbrest_touch);
	cJSON_AddBoolToObject(in, is_left ? "/button/x/click" : "/button/a/click", st.primary_click);
	cJSON_AddBoolToObject(in, is_left ? "/button/x/touch" : "/button/a/touch", st.primary_touch);
	cJSON_AddBoolToObject(in, is_left ? "/button/y/click" : "/button/b/click", st.secondary_click);
	cJSON_AddBoolToObject(in, is_left ? "/button/y/touch" : "/button/b/touch", st.secondary_touch);
	if (is_left) {
		cJSON_AddBoolToObject(in, "/input/menu/click", st.menu_click);
	} else {
		cJSON_AddBoolToObject(in, "/input/system/click", st.system_click);
	}
	return o;
}


/*
 * Command handlers.
 */

// 最小 descriptor(HMD のみ)。fov/解像度の実値は Adapter 殻が config で注入する。hello 応答に埋める。
static void
add_descriptor(struct playspectra_control *c, cJSON *r)
{
	cJSON *desc = cJSON_AddObjectToObject(r, "descriptor");
	cJSON_AddNumberToObject(desc, "protocol_version", PS_PROTOCOL_VERSION);
	cJSON *hmd = cJSON_AddObjectToObject(desc, "hmd");
	cJSON_AddNumberToObject(hmd, "recommended_eye_width", c->recommended_eye_width);
	cJSON_AddNumberToObject(hmd, "recommended_eye_height", c->recommended_eye_height);
	cJSON_AddNumberToObject(hmd, "refresh_hz", c->refresh_hz);
}

static void
handle_hello(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req)
{
	if (!playspectra_proto_hello_version_ok(req, PLAYSPECTRA_PROTOCOL_VERSION)) {
		reply_error(conn->sock, req, "protocol_error", "protocol_version");
		return;
	}
	// role: "writer"(既定) | "observer"。writer は同時1接続のみ(spec §5.3)。
	const cJSON *role = cJSON_GetObjectItemCaseSensitive(req, "role");
	const bool want_observer = cJSON_IsString(role) && strcmp(role->valuestring, "observer") == 0;
	if (!want_observer) {
		// writer 希望。他接続が既に writer を保持中なら拒否(writer 競合 = protocol_error)。
		if (writer_held_by_other(c, conn)) {
			reply_error(conn->sock, req, "protocol_error", "writer_taken");
			return;
		}
		conn->role = PS_ROLE_WRITER;
	} else {
		conn->role = PS_ROLE_OBSERVER;
	}

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "protocol_version", PS_PROTOCOL_VERSION);
	cJSON_AddStringToObject(r, "role_granted", conn->role == PS_ROLE_WRITER ? "writer" : "observer");
	add_descriptor(c, r);
	reply_ok(conn->sock, req, r);
}

// parsed controller(semantic path) → 共有 state。present のときだけ適用(full snapshot)。
// ボタンは左右統一のため x/a→primary, y/b→secondary に写像する。
static void
apply_ctrl(struct playspectra_control *c, enum playspectra_hand hand, const struct playspectra_controller_update *u)
{
	if (!u->present) {
		return;
	}
	struct playspectra_ctrl st;
	memset(&st, 0, sizeof(st));
	st.connected = true;
	if (u->has_grip) {
		st.grip = u->grip;
	}
	if (u->has_aim) {
		st.aim = u->aim;
	}
	for (int i = 0; i < u->input_count; i++) {
		const char *p = u->inputs[i].path;
		double n = u->inputs[i].num;
		bool b = u->inputs[i].flag;
		if (strcmp(p, "/input/trigger/value") == 0) {
			st.trigger = (float)n;
		} else if (strcmp(p, "/input/squeeze/value") == 0) {
			st.squeeze = (float)n;
		} else if (strcmp(p, "/input/thumbstick/x") == 0) {
			st.thumbstick_x = (float)n;
		} else if (strcmp(p, "/input/thumbstick/y") == 0) {
			st.thumbstick_y = (float)n;
		} else if (strcmp(p, "/input/trigger/touch") == 0) {
			st.trigger_touch = b;
		} else if (strcmp(p, "/input/thumbstick/click") == 0) {
			st.thumbstick_click = b;
		} else if (strcmp(p, "/input/thumbstick/touch") == 0) {
			st.thumbstick_touch = b;
		} else if (strcmp(p, "/input/thumbrest/touch") == 0) {
			st.thumbrest_touch = b;
		} else if (strcmp(p, "/button/x/click") == 0 || strcmp(p, "/button/a/click") == 0) {
			st.primary_click = b;
		} else if (strcmp(p, "/button/x/touch") == 0 || strcmp(p, "/button/a/touch") == 0) {
			st.primary_touch = b;
		} else if (strcmp(p, "/button/y/click") == 0 || strcmp(p, "/button/b/click") == 0) {
			st.secondary_click = b;
		} else if (strcmp(p, "/button/y/touch") == 0 || strcmp(p, "/button/b/touch") == 0) {
			st.secondary_touch = b;
		} else if (strcmp(p, "/input/menu/click") == 0) {
			st.menu_click = b;
		} else if (strcmp(p, "/input/system/click") == 0) {
			st.system_click = b;
		}
	}
	playspectra_state_set_ctrl(c->state, hand, &st);
}

// パース済み set_state を共有 state へ適用(spec §2.2/§2.3)。present の device のみ。
static void
apply_parsed_state(struct playspectra_control *c, const struct playspectra_set_state *parsed)
{
	if (parsed->head.present) {
		playspectra_state_set_head(c->state, &parsed->head.pose);
	}
	apply_ctrl(c, PLAYSPECTRA_LEFT, &parsed->left);
	apply_ctrl(c, PLAYSPECTRA_RIGHT, &parsed->right);
}

// realtime: latest-wins by sequence(spec §4)。古い sequence は stale として破棄。
static void
apply_realtime(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req,
               const struct playspectra_set_state *parsed)
{
	uint64_t sequence = parsed->has_sequence ? parsed->sequence : c->sequence + 1;
	if (parsed->has_sequence && sequence <= c->sequence && c->sequence != 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "applied", false);
		cJSON_AddStringToObject(r, "reason", "stale_frame");
		reply_ok(conn->sock, req, r);
		return;
	}
	apply_parsed_state(c, parsed);
	c->sequence = sequence;
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "applied", true);
	cJSON_AddNumberToObject(r, "sequence", (double)sequence);
	reply_ok(conn->sock, req, r);
}

// frame_synchronized: logical_frame ごとに一度適用(spec §4)。
// 新frame→適用 / 同frame同内容→冪等成功 / 同frame異内容→conflict_error / 古frame→stale。
static void
apply_frame_synchronized(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req,
                         const struct playspectra_set_state *parsed)
{
	int64_t lf = parsed->logical_frame;
	uint64_t sig = playspectra_proto_content_sig(parsed);

	switch (playspectra_proto_frame_decide(c->has_frame, c->last_frame, c->frame_sig, lf, sig)) {
	case PLAYSPECTRA_FRAME_STALE: {
		// 既に次の frame を適用済み → 遅延到着は破棄(spec §4)。
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "applied", false);
		cJSON_AddStringToObject(r, "reason", "stale_frame");
		cJSON_AddNumberToObject(r, "logical_frame", (double)lf);
		reply_ok(conn->sock, req, r);
		return;
	}
	case PLAYSPECTRA_FRAME_IDEMPOTENT: {
		// 同一内容の再送 → 冪等成功(再適用しない, spec §4)。
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "applied", true);
		cJSON_AddBoolToObject(r, "idempotent", true);
		cJSON_AddNumberToObject(r, "logical_frame", (double)lf);
		reply_ok(conn->sock, req, r);
		return;
	}
	case PLAYSPECTRA_FRAME_CONFLICT: {
		// 同一 frame・異内容 → conflict_error(spec §5.2/§5.4)。
		cJSON *r = cJSON_CreateObject();
		const cJSON *rid = cJSON_GetObjectItemCaseSensitive(req, "request_id");
		if (cJSON_IsString(rid)) {
			cJSON_AddStringToObject(r, "request_id", rid->valuestring);
		}
		cJSON_AddBoolToObject(r, "ok", false);
		cJSON_AddStringToObject(r, "error_type", "conflict_error");
		cJSON_AddStringToObject(r, "error", "frame_content_mismatch");
		cJSON_AddNumberToObject(r, "logical_frame", (double)lf);
		send_json_line(conn->sock, r);
		cJSON_Delete(r);
		return;
	}
	case PLAYSPECTRA_FRAME_APPLY:
		break;
	}

	// 新しい frame(または最初)→ 適用して記録。
	apply_parsed_state(c, parsed);
	c->has_frame = true;
	c->last_frame = lf;
	c->frame_sig = sig;
	if (parsed->has_sequence) {
		c->sequence = parsed->sequence;
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "applied", true);
	cJSON_AddNumberToObject(r, "logical_frame", (double)lf);
	if (parsed->has_sequence) {
		cJSON_AddNumberToObject(r, "sequence", (double)parsed->sequence);
	}
	reply_ok(conn->sock, req, r);
}

// set_state の hmd.head + left/right を共有 state へ適用。spec §2.2/§2.3/§4。writer のみ。
static void
handle_set_state(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req)
{
	if (conn->role != PS_ROLE_WRITER) {
		// observer / hello 未完了からの書き込みは拒否(writer 排他 = protocol_error, spec §5.3)。
		reply_error(conn->sock, req, "protocol_error", "not_writer");
		return;
	}

	const cJSON *state = cJSON_GetObjectItemCaseSensitive(req, "state");

	struct playspectra_set_state parsed;
	if (playspectra_proto_parse_state(state, &parsed) != PLAYSPECTRA_PARSE_OK) {
		reply_error(conn->sock, req, "validation_error", parsed.error);
		return;
	}

	if (parsed.clock_mode == PLAYSPECTRA_CLOCK_FRAME_SYNCHRONIZED) {
		apply_frame_synchronized(c, conn, req, &parsed);
	} else {
		apply_realtime(c, conn, req, &parsed);
	}
}

// 現在の VirtualDeviceState 全体を返す(spec §5.4 get_state)。observer/writer どちらでも可。
static void
handle_get_state(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req)
{
	struct playspectra_pose head;
	playspectra_state_get_head(c->state, &head);

	cJSON *r = cJSON_CreateObject();
	cJSON *state = cJSON_AddObjectToObject(r, "state");
	cJSON_AddNumberToObject(state, "protocol_version", PS_PROTOCOL_VERSION);
	cJSON_AddNumberToObject(state, "sequence", (double)c->sequence);
	cJSON *hmd = cJSON_AddObjectToObject(state, "hmd");
	cJSON_AddBoolToObject(hmd, "connected", true);
	cJSON_AddItemToObject(hmd, "head", pose_to_json(&head));
	cJSON_AddItemToObject(state, "left", ctrl_to_json(c, PLAYSPECTRA_LEFT));
	cJSON_AddItemToObject(state, "right", ctrl_to_json(c, PLAYSPECTRA_RIGHT));
	reply_ok(conn->sock, req, r);
}

static void
handle_status(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req)
{
	int observers = 0;
	for (int i = 0; i < PS_MAX_CONN; i++) {
		if (c->conns[i].sock != PS_INVALID_SOCKET && c->conns[i].role == PS_ROLE_OBSERVER) {
			observers++;
		}
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "writer_connected", any_writer(c));
	cJSON_AddNumberToObject(r, "observers", observers);
	cJSON_AddNumberToObject(r, "sequence", (double)c->sequence);
	cJSON_AddStringToObject(r, "runtime", c->runtime_name);
	reply_ok(conn->sock, req, r);
}

// reset(spec §5.4, writer のみ): device 状態を Adapter が起動時に書いた値へ初期化する
// (spec §5.3「明示 reset で初期化」= writer 切断時の「保持」の反対)。
// frame_synchronized の重複判定は Adapter ローカルの記録(§4)なので起動直後相当へクリアし、
// 新しい writer が logical_frame を振り直しても直前 frame と stale/conflict にならないようにする。
// sequence は Server 所有の単調増加(§4)なので Adapter からは巻き戻さない。
static void
handle_reset(struct playspectra_control *c, struct ps_conn *conn, const cJSON *req)
{
	if (conn->role != PS_ROLE_WRITER) {
		reply_error(conn->sock, req, "protocol_error", "not_writer");
		return;
	}
	playspectra_state_reset(c->state);
	c->has_frame = false;
	c->last_frame = 0;
	c->frame_sig = 0;
	reply_ok(conn->sock, req, NULL);
}

static void
dispatch_line(struct playspectra_control *c, struct ps_conn *conn, char *line)
{
	cJSON *req = cJSON_Parse(line);
	if (req == NULL) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddBoolToObject(r, "ok", false);
		cJSON_AddStringToObject(r, "error_type", "protocol_error");
		cJSON_AddStringToObject(r, "error", "invalid_json");
		send_json_line(conn->sock, r);
		cJSON_Delete(r);
		return;
	}
	const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
	if (cJSON_IsString(cmd)) {
		if (strcmp(cmd->valuestring, "hello") == 0) {
			handle_hello(c, conn, req);
		} else if (strcmp(cmd->valuestring, "set_state") == 0) {
			handle_set_state(c, conn, req);
		} else if (strcmp(cmd->valuestring, "get_state") == 0) {
			handle_get_state(c, conn, req);
		} else if (strcmp(cmd->valuestring, "status") == 0) {
			handle_status(c, conn, req);
		} else if (strcmp(cmd->valuestring, "reset") == 0) {
			handle_reset(c, conn, req);
		} else {
			reply_error(conn->sock, req, "protocol_error", "unknown_cmd");
		}
	} else {
		reply_error(conn->sock, req, "protocol_error", "missing_cmd");
	}
	cJSON_Delete(req);
}


/*
 * イベント broadcast / 接続の読み取り。
 */

// 1行を接続中の全ハンドシェイク済みクライアント(writer + observer)へ送る。
static void
broadcast_line(struct playspectra_control *c, const char *str, size_t len)
{
	for (int i = 0; i < PS_MAX_CONN; i++) {
		struct ps_conn *cn = &c->conns[i];
		if (cn->sock != PS_INVALID_SOCKET && cn->role != PS_ROLE_NONE) {
			(void)send(cn->sock, str, (int)len, 0);
			(void)send(cn->sock, "\n", 1, 0);
		}
	}
}

// 共有 state に溜まった haptic イベントを全クライアントへ broadcast(spec §5.4 events)。
// キューからは1度だけ pop し、JSON を1回組み立てて全接続へ配る(observer 複数対応)。
static void
broadcast_haptics(struct playspectra_control *c)
{
	struct playspectra_haptic_event e;
	while (playspectra_state_pop_haptic(c->state, &e)) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "event", "haptics");
		cJSON_AddStringToObject(o, "hand", e.hand == PLAYSPECTRA_LEFT ? "left" : "right");
		cJSON_AddNumberToObject(o, "frequency", e.frequency);
		cJSON_AddNumberToObject(o, "amplitude", e.amplitude);
		cJSON_AddNumberToObject(o, "duration_ns", (double)e.duration_ns);
		char *str = cJSON_PrintUnformatted(o);
		if (str != NULL) {
			broadcast_line(c, str, strlen(str));
			cJSON_free(str);
		}
		cJSON_Delete(o);
	}
}

// 1接続から読めるだけ読んで NDJSON 行を dispatch する。切断/エラーなら *closed=true。
static void
service_conn_readable(struct playspectra_control *c, struct ps_conn *conn, bool *closed)
{
	char chunk[4096];
	int n = (int)recv(conn->sock, chunk, sizeof(chunk), 0);
	if (n <= 0) {
		*closed = true; // 切断 or エラー
		return;
	}
	for (int i = 0; i < n; i++) {
		char ch = chunk[i];
		if (ch == '\n') {
			conn->buf[conn->used] = '\0';
			if (conn->used > 0) {
				dispatch_line(c, conn, conn->buf);
			}
			conn->used = 0;
		} else if (conn->used + 1 < PS_LINE_MAX) {
			conn->buf[conn->used++] = ch;
		} else {
			conn->used = 0; // 1 MiB 超過 → 破棄(spec §5.1)
		}
	}
}

static void *
control_thread(void *ptr)
{
	struct playspectra_control *c = (struct playspectra_control *)ptr;
	while (ps_thread_is_running(&c->oth)) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(c->listen_sock, &rfds);
		ps_socket_t maxfd = c->listen_sock;
		for (int i = 0; i < PS_MAX_CONN; i++) {
			ps_socket_t s = c->conns[i].sock;
			if (s != PS_INVALID_SOCKET) {
				FD_SET(s, &rfds);
				if (s > maxfd) {
					maxfd = s; // Windows では nfds は無視される
				}
			}
		}

		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200 * 1000; // 200 ms(stop フラグ / haptic を定期チェック)
		int r = select((int)maxfd + 1, &rfds, NULL, NULL, &tv);
		if (!ps_thread_is_running(&c->oth)) {
			break; // stop during select
		}
		if (r < 0) {
			continue; // select エラー(EINTR 等): 次周回へ(200ms timeout が実質の休止)
		}

		// haptic は timeout / 活動どちらでも流す(接続中の全 observer/writer へ)。
		broadcast_haptics(c);
		if (r == 0) {
			continue; // timeout: 新規接続も読み取りも無し
		}

		// 新規接続を受け付ける。
		if (FD_ISSET(c->listen_sock, &rfds)) {
			ps_socket_t conn = accept(c->listen_sock, NULL, NULL);
			if (conn != PS_INVALID_SOCKET) {
				struct ps_conn *slot = alloc_conn(c, conn);
				if (slot == NULL) {
					ps_close_socket(conn); // 満杯 or buf 確保失敗
					ctl_log(c, PS_LOG_INFO, "PlaySpectra control: connection rejected (full)");
				} else {
					ctl_log(c, PS_LOG_INFO, "PlaySpectra control: client connected");
				}
			}
		}

		// 既存接続の読み取り。
		for (int i = 0; i < PS_MAX_CONN; i++) {
			struct ps_conn *cn = &c->conns[i];
			if (cn->sock == PS_INVALID_SOCKET || !FD_ISSET(cn->sock, &rfds)) {
				continue;
			}
			bool closed = false;
			service_conn_readable(c, cn, &closed);
			if (closed) {
				bool was_writer = (cn->role == PS_ROLE_WRITER);
				free_conn(cn);
				// writer 切断でも state は保持(device は消さない, spec §5.3)。
				ctl_log(c, PS_LOG_INFO,
				        was_writer ? "PlaySpectra control: writer disconnected (state held)"
				                   : "PlaySpectra control: observer disconnected");
			}
		}
	}
	return NULL;
}


/*
 * 'Exported' functions.
 */

struct playspectra_control *
playspectra_control_start(struct playspectra_state *state, const struct playspectra_control_config *config)
{
	if (config == NULL || config->port == 0) {
		return NULL; // port は殻が解決済みの実値を渡す(playspectra_control.h)
	}

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return NULL;
	}
#endif

	ps_socket_t ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls == PS_INVALID_SOCKET) {
		return NULL;
	}
	int yes = 1;
	(void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 のみ
	addr.sin_port = htons(config->port);
	if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, PS_MAX_CONN) != 0) {
		ps_close_socket(ls);
		return NULL;
	}

	struct playspectra_control *c = calloc(1, sizeof(struct playspectra_control));
	if (c == NULL) {
		ps_close_socket(ls);
		return NULL;
	}
	c->state = state;
	playspectra_state_ref(state);
	c->listen_sock = ls;
	c->port = config->port;
	c->sequence = 0;
	snprintf(c->runtime_name, sizeof(c->runtime_name), "%s",
	         config->runtime_name != NULL ? config->runtime_name : "unknown");
	c->recommended_eye_width = config->recommended_eye_width;
	c->recommended_eye_height = config->recommended_eye_height;
	c->refresh_hz = config->refresh_hz;
	c->log_fn = config->log_fn;
	c->log_user = config->log_user;
	// calloc の 0 は Linux で fd=0(stdin)と衝突するため、全スロットを明示的に空にする。
	for (int i = 0; i < PS_MAX_CONN; i++) {
		c->conns[i].sock = PS_INVALID_SOCKET;
		c->conns[i].buf = NULL;
	}

	ps_thread_init(&c->oth);
	if (ps_thread_start(&c->oth, control_thread, c) != 0) {
		ps_close_socket(ls);
		ps_thread_destroy(&c->oth);
		playspectra_state_unref(c->state);
		free(c);
		return NULL;
	}
	ctl_log(c, PS_LOG_NOTICE, "PlaySpectra control channel listening on 127.0.0.1:%u", (unsigned)c->port);
	return c;
}

void
playspectra_control_stop(struct playspectra_control *c)
{
	if (c == NULL) {
		return;
	}
	// stop フラグを立ててスレッド join。select が 200ms 以内に is_running=false を検知して抜けるので、
	// close() で accept を叩き起こす必要はない(Linux では不確実なため)。
	ps_thread_stop_and_wait(&c->oth);
	ps_thread_destroy(&c->oth);
	if (c->listen_sock != PS_INVALID_SOCKET) {
		ps_close_socket(c->listen_sock);
	}
	// 残っている接続を閉じ、行バッファを解放(スレッドは既に停止済み)。
	for (int i = 0; i < PS_MAX_CONN; i++) {
		if (c->conns[i].sock != PS_INVALID_SOCKET) {
			ps_close_socket(c->conns[i].sock);
		}
		free(c->conns[i].buf);
	}
#ifdef _WIN32
	WSACleanup();
#endif
	playspectra_state_unref(c->state);
	free(c);
}
