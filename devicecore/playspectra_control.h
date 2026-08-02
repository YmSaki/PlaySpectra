// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

/*!
 * @file
 * @brief  Public interface of the PlaySpectra NDJSON control channel (spec §5).
 *         opaque。start で accept ループのスレッドを立てる。ランタイム固有の値
 *         (ポート・runtime 名・descriptor・ログ出力)は全て config で Adapter 殻から
 *         注入する — core は環境変数もランタイム APIも読まない。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct playspectra_state;
struct playspectra_control;

/*!
 * core が発するログのレベル。config.log_fn へ渡される。出力するかの判断
 * (レベルゲート含む)は Adapter 殻の責務。
 */
enum ps_log_level
{
	PS_LOG_ERROR = 0,
	PS_LOG_NOTICE, // 常時表示したい起動メッセージ(listening 等)
	PS_LOG_INFO,   // 接続/切断などの詳細(既定では抑制される想定)
};

/*!
 * 制御チャネルの起動設定。Adapter 殻が全フィールドを埋めて渡す。
 */
struct playspectra_control_config
{
	uint16_t port;            //!< listen ポート(殻が環境変数等を解決済みの実値。0 不可)
	const char *runtime_name; //!< status 応答の "runtime"(例: "monado")。core が内部へコピーする
	int recommended_eye_width;  //!< hello descriptor(spec §5.4)
	int recommended_eye_height; //!< 同上
	int refresh_hz;             //!< 同上
	//! ログ出力(NULL なら無音)。msg は整形済みの1行。
	void (*log_fn)(void *user, enum ps_log_level level, const char *msg);
	void *log_user;
};

/*!
 * 制御チャネルを開始する。set_state は @p state に書き込む(Adapter のデバイスがそこから
 * 読む)。制御チャネルは state を1つ ref する。失敗時は NULL(デバイスは初期 pose のまま動く)。
 */
struct playspectra_control *
playspectra_control_start(struct playspectra_state *state, const struct playspectra_control_config *config);

/*!
 * 制御チャネルを停止・解放する。NULL 安全。
 */
void
playspectra_control_stop(struct playspectra_control *ctl);

#ifdef __cplusplus
}
#endif
