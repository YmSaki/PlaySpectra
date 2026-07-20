// PlaySpectra headless OpenXR pose-reader (M2.2/M2.3 verification).
// XR_MND_headless -> no graphics binding/compositor swapchains. Uses
// XR_KHR_convert_timespec_time to build a valid XrTime from the monotonic clock, so
// xrLocateSpace(VIEW in STAGE) reads the head pose without depending on frame timing.
// With PLAYSPECTRA_ENABLE=1 + XRT_COMPOSITOR_NULL=1 and the Monado in-process runtime,
// this reads the PlaySpectra virtual HMD pose. Sending NDJSON set_state to
// 127.0.0.1:52702 mid-run changes the printed pose (M2.3).
//
// Build (WSL): gcc playspectra_headless_probe.c -lopenxr_loader -o ps_probe_xr

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

int
main(int argc, char **argv)
{
	int iters = (argc > 1) ? atoi(argv[1]) : 60;

	const char *exts[] = {"XR_MND_headless", "XR_KHR_convert_timespec_time"};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ici.applicationInfo.applicationName, "ps_headless_probe");
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

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = sys;
	sci.next = NULL; // headless
	XrSession sess;
	CHK(xrCreateSession(inst, &sci, &sess));

	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.poseInReferenceSpace.orientation.w = 1.0f;
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	XrSpace stage;
	CHK(xrCreateReferenceSpace(sess, &rci, &stage));
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XrSpace view;
	CHK(xrCreateReferenceSpace(sess, &rci, &view));

	int running = 0, printed = 0;
	for (int i = 0; i < iters; i++) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrEventDataSessionStateChanged *ssc = (XrEventDataSessionStateChanged *)&ev;
				if (ssc->state == XR_SESSION_STATE_READY && !running) {
					XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
					sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (xrBeginSession(sess, &sbi) == XR_SUCCESS) {
						running = 1;
						printf("[probe] session READY -> begun\n");
						fflush(stdout);
					}
				} else if (ssc->state == XR_SESSION_STATE_STOPPING) {
					xrEndSession(sess);
					running = 0;
				}
			}
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		}

		if (running) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			XrTime t = 0;
			if (toTime(inst, &ts, &t) == XR_SUCCESS && t != 0) {
				XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
				XrResult lr = xrLocateSpace(view, stage, t, &loc);
				XrVector3f p = loc.pose.position;
				XrQuaternionf q = loc.pose.orientation;
				printf("[probe] iter=%d lr=%d flags=0x%x pos=(%.3f, %.3f, %.3f) "
				       "quat=(%.3f, %.3f, %.3f, %.3f)\n",
				       i, lr, (unsigned)loc.locationFlags, p.x, p.y, p.z, q.x, q.y, q.z, q.w);
				fflush(stdout);
				printed++;
			}
		}
		usleep(120 * 1000);
	}

	printf("[probe] done, %d samples\n", printed);
	fflush(stdout);
	if (running) {
		xrEndSession(sess);
	}
	xrDestroySession(sess);
	xrDestroyInstance(inst);
	return printed > 0 ? 0 : 2;
}
