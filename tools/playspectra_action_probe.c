// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// PlaySpectra controller-input E2E probe.
// Sets up an OpenXR action set bound to /interaction_profiles/oculus/touch_controller,
// runs the frame loop (to progress the session to FOCUSED), then each frame syncs actions
// and prints the left hand thumbstick / trigger / grip pose. With PLAYSPECTRA_ENABLE=1 +
// XRT_COMPOSITOR_NULL=1, sending set_state to :52702 changes what this app reads.
// Also logs session state transitions so we can see how far the null compositor progresses.
//
// Build (WSL): gcc playspectra_action_probe.c -lopenxr_loader -o ps_action_probe

#define XR_USE_TIMESPEC 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHK(x)                                                                                                         \
	do {                                                                                                           \
		XrResult _r = (x);                                                                                     \
		if (_r != XR_SUCCESS) {                                                                                 \
			printf("FAIL: %s -> %d\n", #x, _r);                                                             \
			fflush(stdout);                                                                                 \
			return 1;                                                                                       \
		}                                                                                                      \
	} while (0)

static const char *
state_str(XrSessionState s)
{
	switch (s) {
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	default: return "?";
	}
}

int
main(int argc, char **argv)
{
	int iters = (argc > 1) ? atoi(argv[1]) : 80;

	const char *exts[] = {"XR_MND_headless", "XR_KHR_convert_timespec_time"};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ici.applicationInfo.applicationName, "ps_action_probe");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	XrInstance inst;
	CHK(xrCreateInstance(&ici, &inst));

	PFN_xrConvertTimespecTimeToTimeKHR toTime = NULL;
	CHK(xrGetInstanceProcAddr(inst, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *)&toTime));

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	CHK(xrGetSystem(inst, &sgi, &sys));

	// --- Action set + actions (left hand) ---
	XrActionSet aset;
	XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(asci.actionSetName, "ps");
	strcpy(asci.localizedActionSetName, "PlaySpectra");
	CHK(xrCreateActionSet(inst, &asci, &aset));

	XrPath leftHand;
	CHK(xrStringToPath(inst, "/user/hand/left", &leftHand));

	XrAction gripA, thumbA, trigA, hapticA;
	XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
	aci.countSubactionPaths = 1;
	aci.subactionPaths = &leftHand;
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	strcpy(aci.actionName, "grip");
	strcpy(aci.localizedActionName, "grip");
	CHK(xrCreateAction(aset, &aci, &gripA));
	aci.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
	strcpy(aci.actionName, "thumbstick");
	strcpy(aci.localizedActionName, "thumbstick");
	CHK(xrCreateAction(aset, &aci, &thumbA));
	aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
	strcpy(aci.actionName, "trigger");
	strcpy(aci.localizedActionName, "trigger");
	CHK(xrCreateAction(aset, &aci, &trigA));
	aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	strcpy(aci.actionName, "haptic");
	strcpy(aci.localizedActionName, "haptic");
	CHK(xrCreateAction(aset, &aci, &hapticA));

	XrPath gripPath, thumbPath, trigPath, hapticPath, profile;
	CHK(xrStringToPath(inst, "/user/hand/left/input/grip/pose", &gripPath));
	CHK(xrStringToPath(inst, "/user/hand/left/input/thumbstick", &thumbPath));
	CHK(xrStringToPath(inst, "/user/hand/left/input/trigger/value", &trigPath));
	CHK(xrStringToPath(inst, "/user/hand/left/output/haptic", &hapticPath));
	CHK(xrStringToPath(inst, "/interaction_profiles/oculus/touch_controller", &profile));
	XrActionSuggestedBinding binds[] = {
	    {gripA, gripPath}, {thumbA, thumbPath}, {trigA, trigPath}, {hapticA, hapticPath}};
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	sb.interactionProfile = profile;
	sb.suggestedBindings = binds;
	sb.countSuggestedBindings = 4;
	CHK(xrSuggestInteractionProfileBindings(inst, &sb));

	// --- Session (headless) ---
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = sys;
	sci.next = NULL;
	XrSession sess;
	CHK(xrCreateSession(inst, &sci, &sess));

	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &aset;
	CHK(xrAttachSessionActionSets(sess, &attach));

	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.poseInReferenceSpace.orientation.w = 1.0f;
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	XrSpace stage;
	CHK(xrCreateReferenceSpace(sess, &rci, &stage));

	XrActionSpaceCreateInfo aspci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
	aspci.action = gripA;
	aspci.subactionPath = leftHand;
	aspci.poseInActionSpace.orientation.w = 1.0f;
	XrSpace gripSpace;
	CHK(xrCreateActionSpace(sess, &aspci, &gripSpace));

	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	int running = 0, reads = 0;

	for (int i = 0; i < iters; i++) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrEventDataSessionStateChanged *ssc = (XrEventDataSessionStateChanged *)&ev;
				state = ssc->state;
				printf("[state] -> %s\n", state_str(state));
				fflush(stdout);
				if (state == XR_SESSION_STATE_READY && !running) {
					XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
					sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (xrBeginSession(sess, &sbi) == XR_SUCCESS) {
						running = 1;
					}
				} else if (state == XR_SESSION_STATE_STOPPING) {
					xrEndSession(sess);
					running = 0;
				}
			}
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		}

		if (running) {
			// Pump a frame so the runtime can progress the session state.
			XrFrameState fs = {XR_TYPE_FRAME_STATE};
			XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
			XrResult wr = xrWaitFrame(sess, &fwi, &fs);
			if (wr == XR_SUCCESS) {
				XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
				xrBeginFrame(sess, &fbi);
				XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
				fei.displayTime = fs.predictedDisplayTime;
				fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				fei.layerCount = 0;
				xrEndFrame(sess, &fei);
			}

			if (state == XR_SESSION_STATE_FOCUSED) {
				XrActiveActionSet aas = {aset, XR_NULL_PATH};
				XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
				syncInfo.countActiveActionSets = 1;
				syncInfo.activeActionSets = &aas;
				xrSyncActions(sess, &syncInfo);

				XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
				gi.subactionPath = leftHand;
				XrActionStateVector2f tsv = {XR_TYPE_ACTION_STATE_VECTOR2F};
				gi.action = thumbA;
				xrGetActionStateVector2f(sess, &gi, &tsv);
				XrActionStateFloat trv = {XR_TYPE_ACTION_STATE_FLOAT};
				gi.action = trigA;
				xrGetActionStateFloat(sess, &gi, &trv);

				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				XrTime t = 0;
				toTime(inst, &ts, &t);
				XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
				xrLocateSpace(gripSpace, stage, t, &loc);

				printf("[act] thumb(active=%d %.2f,%.2f) trig(active=%d %.2f) "
				       "grip(flags=0x%x pos=%.3f,%.3f,%.3f)\n",
				       tsv.isActive, tsv.currentState.x, tsv.currentState.y, trv.isActive,
				       trv.currentState, (unsigned)loc.locationFlags, loc.pose.position.x,
				       loc.pose.position.y, loc.pose.position.z);
				fflush(stdout);
				reads++;

				// haptics 逆方向の検証: 振動を周期的に適用する。
				// PlaySpectra 側は controller.set_output -> 共有state -> 制御チャネルが
				// {"event":"haptics",...} を接続中の全 observer/writer へ broadcast する。
				// 一度きりだと observer の接続完了前に fire して取りこぼす(broadcast は
				// pop して即破棄する)ため、reads>=2 から 15 read ごとに繰り返し発火させる。
				if (reads >= 2 && (reads - 2) % 15 == 0) {
					XrHapticActionInfo hai = {XR_TYPE_HAPTIC_ACTION_INFO};
					hai.action = hapticA;
					hai.subactionPath = leftHand;
					XrHapticVibration hv = {XR_TYPE_HAPTIC_VIBRATION};
					hv.amplitude = 0.8f;
					hv.duration = 100000000; // 100 ms in ns
					hv.frequency = 0;        // unspecified
					xrApplyHapticFeedback(sess, &hai, (const XrHapticBaseHeader *)&hv);
					printf("[act] applied haptic feedback (left)\n");
					fflush(stdout);
				}
			}
		}
		usleep(120 * 1000);
	}

	printf("[act] done, %d focused reads\n", reads);
	if (running) {
		xrEndSession(sess);
	}
	xrDestroySession(sess);
	xrDestroyInstance(inst);
	return reads > 0 ? 0 : 2;
}
