package setuphelper

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

func TestTransformVCXProjPreservesWin32ProjAndConvertsPlatform(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sample.vcxproj")
	if err := os.WriteFile(path, []byte("\ufeffWin32Proj Win32 win32"), 0o600); err != nil {
		t.Fatal(err)
	}
	out, err := TransformVCXProj(path)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(out)
	if string(data) != "Win32Proj x64 win64" || filepath.Base(out) != "sample_x64.vcxproj" {
		t.Fatalf("out=%q data=%q", out, data)
	}
}

func TestDeployHelloVRCopiesRuntimeAndAssets(t *testing.T) {
	root := t.TempDir()
	samples := filepath.Join(root, "samples")
	for path, content := range map[string]string{
		"bin/cube_texture.png":                        "texture",
		"bin/hellovr_actions.json":                    "actions",
		"bin/hellovr_bindings_generic.json":           "generic",
		"bin/hellovr_bindings_vive_controller.json":   "vive",
		"bin/shaders/scene.hlsl":                      "shader",
		"bin/win64/hellovr_dx12_x64.exe":              "exe",
		"../third_party/monado/bin/SDL2.dll":          "sdl",
		"../third_party/opencomposite/openvr_api.dll": "openvr",
	} {
		full := filepath.Join(samples, filepath.FromSlash(path))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	destination := filepath.Join(root, "deployed")
	names, err := DeployHelloVR(samples, destination, root)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(names, ",") != "SDL2.dll,hellovr_dx12.exe,openvr_api.dll" {
		t.Fatalf("win64 entries=%v", names)
	}
	for path, want := range map[string]string{
		"cube_texture.png": "texture", "shaders/scene.hlsl": "shader",
		"win64/hellovr_dx12.exe": "exe", "win64/SDL2.dll": "sdl", "win64/openvr_api.dll": "openvr",
	} {
		data, err := os.ReadFile(filepath.Join(destination, filepath.FromSlash(path)))
		if err != nil || string(data) != want {
			t.Fatalf("%s=%q err=%v", path, data, err)
		}
	}
}

func TestExtractMonadoSelectsInstallTreeAndRejectsTraversal(t *testing.T) {
	zipPath := filepath.Join(t.TempDir(), "monado.zip")
	writeZip(t, zipPath, map[string]string{
		"install/bin/monado-service.exe":          "service",
		"install/share/steamvr-monado/driver.dll": "excluded",
		"build/log.txt":                           "excluded",
	})
	destination := filepath.Join(t.TempDir(), "monado")
	count, err := ExtractMonado(zipPath, destination)
	if err != nil || count != 1 {
		t.Fatalf("count=%d err=%v", count, err)
	}
	data, err := os.ReadFile(filepath.Join(destination, "bin", "monado-service.exe"))
	if err != nil || string(data) != "service" {
		t.Fatalf("data=%q err=%v", data, err)
	}

	unsafeZip := filepath.Join(t.TempDir(), "unsafe.zip")
	writeZip(t, unsafeZip, map[string]string{"install/../../escaped": "bad"})
	if _, err := ExtractMonado(unsafeZip, filepath.Join(t.TempDir(), "target")); err == nil {
		t.Fatal("path traversal was accepted")
	}
}

// TestExtractMonadoRejectsAnArchiveWithNothingToExtract covers the shape a
// broken Monado CI artifact actually takes: the download succeeds and the zip
// parses, but the layout moved and none of it lands under install/. Reporting
// that as a successful extraction of zero files leaves the caller to install a
// runtime that is not there.
func TestExtractMonadoRejectsAnArchiveWithNothingToExtract(t *testing.T) {
	zipPath := filepath.Join(t.TempDir(), "monado.zip")
	writeZip(t, zipPath, map[string]string{
		"build/bin/monado-service.exe":            "moved out of install/",
		"install/share/steamvr-monado/driver.dll": "excluded by design",
	})
	destination := filepath.Join(t.TempDir(), "monado")
	count, err := ExtractMonado(zipPath, destination)
	if err == nil {
		t.Fatalf("an archive with no install/ tree extracted %d files without an error", count)
	}
	if !strings.Contains(err.Error(), zipPath) || !strings.Contains(err.Error(), "0 files") {
		t.Fatalf("error names neither the archive nor the empty result: %v", err)
	}
}

func TestCheckPEX64(t *testing.T) {
	valid := make([]byte, 0x1001)
	copy(valid, "MZ")
	binary.LittleEndian.PutUint32(valid[0x3c:], 0x100)
	copy(valid[0x100:], "PE\x00\x00")
	binary.LittleEndian.PutUint16(valid[0x104:], 0x8664)
	path := filepath.Join(t.TempDir(), "openvr_api.dll")
	if err := os.WriteFile(path, valid, 0o600); err != nil {
		t.Fatal(err)
	}
	if size, err := CheckPEX64(path); err != nil || size != int64(len(valid)) {
		t.Fatalf("size=%d err=%v", size, err)
	}
	valid[0x104] = 0x4c
	valid[0x105] = 0x01
	if err := os.WriteFile(path, valid, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := CheckPEX64(path); err == nil {
		t.Fatal("x86 PE was accepted")
	}
}

// TestCheckPEX64RejectsTheShapesOfABrokenDownload exercises the guard from the
// side it exists for. A PE header can be perfectly well formed in a file that
// stopped downloading early, so the size floor carries as much weight as the
// signature, and e_lfanew is attacker- and corruption-controlled: it is read
// out of the file and then used to index it.
func TestCheckPEX64RejectsTheShapesOfABrokenDownload(t *testing.T) {
	tests := []struct {
		name      string
		size      int
		peOffset  int
		signature string
		wantError bool
	}{
		{name: "exactly the size floor", size: 0x1000, peOffset: 0x100, signature: "MZ", wantError: true},
		{name: "one byte over the size floor", size: 0x1001, peOffset: 0x100, signature: "MZ"},
		{name: "e_lfanew points past the end", size: 0x1001, peOffset: 0x1000, signature: "MZ", wantError: true},
		{name: "e_lfanew leaves no room for the machine field", size: 0x1001, peOffset: 0x0FFC, signature: "MZ", wantError: true},
		{name: "no DOS signature", size: 0x1001, peOffset: 0x100, signature: "ZM", wantError: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			data := make([]byte, test.size)
			copy(data, test.signature)
			binary.LittleEndian.PutUint32(data[0x3c:0x40], uint32(test.peOffset))
			if test.peOffset+6 <= test.size {
				copy(data[test.peOffset:], "PE\x00\x00")
				binary.LittleEndian.PutUint16(data[test.peOffset+4:], 0x8664)
			}
			path := filepath.Join(t.TempDir(), "openvr_api.dll")
			if err := os.WriteFile(path, data, 0o600); err != nil {
				t.Fatal(err)
			}
			size, err := CheckPEX64(path)
			if (err != nil) != test.wantError {
				t.Fatalf("err=%v, wantError=%v", err, test.wantError)
			}
			if size != int64(test.size) {
				t.Errorf("size=%d, want %d reported even on rejection", size, test.size)
			}
		})
	}
}

// TestExtractMonadoRejectsAResponseThatIsNotAnArchive covers the other way the
// artifact download goes wrong: an expired URL or a proxy answers with an HTML
// error page, which the downloader stores under the .zip name unexamined.
func TestExtractMonadoRejectsAResponseThatIsNotAnArchive(t *testing.T) {
	path := filepath.Join(t.TempDir(), "monado.zip")
	if err := os.WriteFile(path, []byte("<!doctype html>\n<title>404 Not Found</title>\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if count, err := ExtractMonado(path, filepath.Join(t.TempDir(), "monado")); err == nil {
		t.Fatalf("an HTML error page was extracted as an archive: count=%d", count)
	}
}

// TestDeployHelloVRAbortsWithoutClaimingADeployment pins that a missing source
// stops the deployment and, more importantly, that the caller is never handed a
// file list it could mistake for a complete one. The copy is not rolled back,
// so the list is the only signal that separates a finished deployment from an
// abandoned one.
func TestDeployHelloVRAbortsWithoutClaimingADeployment(t *testing.T) {
	newSamples := func(t *testing.T, omit ...string) (string, string, string) {
		t.Helper()
		root := t.TempDir()
		samples := filepath.Join(root, "samples")
		for _, path := range []string{
			"bin/cube_texture.png", "bin/hellovr_actions.json",
			"bin/hellovr_bindings_generic.json", "bin/hellovr_bindings_vive_controller.json",
			"bin/shaders/scene.hlsl", "bin/win64/hellovr_dx12_x64.exe",
			"../third_party/monado/bin/SDL2.dll", "../third_party/opencomposite/openvr_api.dll",
		} {
			skip := false
			for _, prefix := range omit {
				skip = skip || strings.HasPrefix(path, prefix)
			}
			if skip {
				continue
			}
			full := filepath.Join(samples, filepath.FromSlash(path))
			if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(full, []byte("fixture"), 0o600); err != nil {
				t.Fatal(err)
			}
		}
		return samples, filepath.Join(root, "deployed"), root
	}

	t.Run("a missing asset stops it before win64 is populated", func(t *testing.T) {
		samples, destination, root := newSamples(t, "bin/cube_texture.png")
		names, err := DeployHelloVR(samples, destination, root)
		if err == nil {
			t.Fatalf("deployment succeeded without its assets: %v", names)
		}
		if names != nil {
			t.Errorf("a failed deployment returned a file list: %v", names)
		}
		if !strings.Contains(err.Error(), "cube_texture.png") {
			t.Errorf("error does not name the missing source: %v", err)
		}
		assertEmptyDir(t, filepath.Join(destination, "win64"))
	})

	// Every win64 source is missing, so the abort happens on whichever the map
	// yields first and the assertion stays independent of that order.
	t.Run("missing runtime binaries stop it too", func(t *testing.T) {
		samples, destination, root := newSamples(t, "bin/win64", "../third_party")
		names, err := DeployHelloVR(samples, destination, root)
		if err == nil {
			t.Fatalf("deployment succeeded without its runtime binaries: %v", names)
		}
		if names != nil {
			t.Errorf("a failed deployment returned a file list: %v", names)
		}
		assertEmptyDir(t, filepath.Join(destination, "win64"))
	})
}

func assertEmptyDir(t *testing.T, path string) {
	t.Helper()
	entries, err := os.ReadDir(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	if len(entries) != 0 {
		names := make([]string, 0, len(entries))
		for _, entry := range entries {
			names = append(names, entry.Name())
		}
		t.Errorf("%s holds %v after an aborted deployment", path, names)
	}
}

func TestWaitTCPAndTimeout(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := WaitTCP(ctx, listener.Addr().String(), time.Millisecond); err != nil {
		t.Fatal(err)
	}

	ctx, cancel = context.WithTimeout(context.Background(), 15*time.Millisecond)
	defer cancel()
	err = WaitTCP(ctx, "127.0.0.1:1", time.Millisecond)
	if err == nil {
		t.Fatal("closed port did not time out")
	}
	// When a service never comes up, this message is the only diagnostic the
	// setup scripts surface: which endpoint was waited on, and why the last
	// attempt to reach it failed. Reducing it to "timed out" leaves the reader
	// unable to tell a wrong port from a service that crashed on startup.
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("the timeout does not wrap the context error: %v", err)
	}
	for _, want := range []string{"127.0.0.1:1", "last dial:"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("wait error is missing %q: %v", want, err)
		}
	}
}

func TestCompareCapturesRequiresDistinctBaselineAndPost(t *testing.T) {
	dir := t.TempDir()
	baseline := filepath.Join(dir, "rec_0001.png")
	post := filepath.Join(dir, "rec_0002.png")
	_ = os.WriteFile(baseline, []byte("baseline"), 0o600)
	_ = os.WriteFile(post, []byte("post"), 0o600)
	result, err := CompareCaptures(dir, baseline, post)
	if err != nil || !result.OK || result.Frames != 2 || result.Distinct != 2 || result.BaselineHash == result.PostHash {
		t.Fatalf("result=%+v err=%v", result, err)
	}
	_ = os.WriteFile(post, []byte("baseline"), 0o600)
	result, err = CompareCaptures(dir, baseline, post)
	if err != nil || result.OK || result.Distinct != 1 {
		t.Fatalf("result=%+v err=%v", result, err)
	}
}

// helloXRSources is the upstream hello_xr sample reduced to the lines the
// patcher anchors on.
func helloXRSources() map[string]string {
	return map[string]string{
		"logger.cpp": "#include <sstream>\n",
		"graphicsplugin_d3d11.cpp": `#include "pch.h"
uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override { return 1; }
const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, (DXGI_FORMAT)swapchainFormat);
depthDesc.SampleDesc.Count = 1;
CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);
int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        // List of supported color swapchain formats.
`,
		"graphicsplugin_d3d12.cpp": `#include "pch.h"
    void UpdateOptions(const std::shared_ptr<Options>& options) override { m_clearColor = options->GetBackgroundClearColor(); }
depthDesc.SampleDesc.Count = 1;
pipelineStateDesc.SampleDesc = {1, 0};
int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        // List of supported color swapchain formats.
`,
	}
}

// writeHelloXRTree lays the sources out where PatchHelloXR looks for them and
// returns the directory it wrote them to.
func writeHelloXRTree(t *testing.T, root string, files map[string]string) string {
	t.Helper()
	dir := filepath.Join(root, "src", "tests", "hello_xr")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, content := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestPatchHelloXRMatchesPythonPatchAndIsIdempotent(t *testing.T) {
	root := t.TempDir()
	dir := writeHelloXRTree(t, root, helloXRSources())
	changed, err := PatchHelloXR(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(changed) != 14 {
		t.Fatalf("changed markers=%v", changed)
	}
	for name, markers := range map[string][]string{
		"logger.cpp":               {"#include <chrono>"},
		"graphicsplugin_d3d11.cpp": {"HELLO_XR_SAMPLE_COUNT", "HELLO_XR_HDR", "HELLO_XR_TYPELESS", "R8G8B8A8_TYPELESS"},
		"graphicsplugin_d3d12.cpp": {"HELLO_XR_SAMPLE_COUNT", "psoSampleCountEnv", "HELLO_XR_HDR"},
	} {
		data, _ := os.ReadFile(filepath.Join(dir, name))
		for _, marker := range markers {
			if !strings.Contains(string(data), marker) {
				t.Fatalf("%s missing %s", name, marker)
			}
		}
	}
	changed, err = PatchHelloXR(root)
	if err != nil || len(changed) != 0 {
		t.Fatalf("second patch changed=%v err=%v", changed, err)
	}
}

// TestPatchHelloXRRefusesAnAnchorItCannotPlaceExactlyOnce covers what an OpenXR
// SDK update does to this patcher: the code it anchors on is rewritten or
// duplicated. Guessing a location would produce a hello_xr that builds and runs
// but never honours HELLO_XR_SAMPLE_COUNT, and the D3D capture downstream would
// then fail for no visible reason -- so the patcher has to refuse, name the file
// and the marker it could not place, and leave that file exactly as it found it.
func TestPatchHelloXRRefusesAnAnchorItCannotPlaceExactlyOnce(t *testing.T) {
	tests := []struct {
		name       string
		file       string
		content    string
		wantMarker string
		wantCount  string
	}{
		{
			name:       "the anchor is gone",
			file:       "graphicsplugin_d3d11.cpp",
			content:    "#include \"pch.h\"\n// upstream rewrote the sample count accessor\n",
			wantMarker: "HELLO_XR_SAMPLE_COUNT",
			wantCount:  "0 times",
		},
		{
			name:       "the anchor is ambiguous",
			file:       "logger.cpp",
			content:    "#include <sstream>\n#include <sstream>\n",
			wantMarker: "#include <chrono>",
			wantCount:  "2 times",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			root := t.TempDir()
			files := helloXRSources()
			files[test.file] = test.content
			dir := writeHelloXRTree(t, root, files)

			path := filepath.Join(dir, test.file)
			before, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			_, err = PatchHelloXR(root)
			if err == nil {
				t.Fatal("the patcher accepted an anchor it could not place exactly once")
			}
			for _, want := range []string{path, test.wantMarker, test.wantCount} {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("error is missing %q: %v", want, err)
				}
			}
			after, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(before, after) {
				t.Errorf("the rejected file was rewritten anyway:\nbefore %q\nafter  %q", before, after)
			}
		})
	}
}

// TestPatchHelloXRLeavesEarlierFilesPatchedAfterALaterFailure pins the fact that
// the patcher is atomic per file and not across files. That is only safe because
// patching is idempotent: a rerun after the anchor is repaired skips the files
// that already carry their markers, which the idempotency test above proves.
func TestPatchHelloXRLeavesEarlierFilesPatchedAfterALaterFailure(t *testing.T) {
	root := t.TempDir()
	files := helloXRSources()
	files["graphicsplugin_d3d11.cpp"] = "#include \"pch.h\"\n// upstream rewrote the sample count accessor\n"
	dir := writeHelloXRTree(t, root, files)

	changed, err := PatchHelloXR(root)
	if err == nil {
		t.Fatal("the patcher accepted a missing anchor")
	}
	if len(changed) != 1 || changed[0] != "#include <chrono>" {
		t.Fatalf("markers reported before the failure = %v", changed)
	}
	logger, err := os.ReadFile(filepath.Join(dir, "logger.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(logger), "#include <chrono>") {
		t.Errorf("logger.cpp was rolled back, which the reported markers deny: %q", logger)
	}
}

func TestPatchHelloXRReportsAMissingSourceTree(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "no-openxr-sdk")
	changed, err := PatchHelloXR(absent)
	if err == nil {
		t.Fatalf("a source root with no hello_xr sample patched %v", changed)
	}
	if !strings.Contains(err.Error(), "logger.cpp") {
		t.Errorf("error does not name the file it could not read: %v", err)
	}
}

func TestRepositoryHasNoOwnedPythonRuntime(t *testing.T) {
	root := filepath.Clean("..")
	var sources []string
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		relative, _ := filepath.Rel(root, path)
		if entry.IsDir() {
			if relative == filepath.FromSlash("runtime/monado-playspectra") || entry.Name() == ".git" || entry.Name() == "node_modules" || entry.Name() == "build" || entry.Name() == "third_party" {
				return filepath.SkipDir
			}
			return nil
		}
		if strings.EqualFold(filepath.Ext(path), ".py") {
			sources = append(sources, filepath.ToSlash(relative))
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(sources) != 0 {
		t.Fatalf("owned Python sources remain: %v", sources)
	}

	legacyRuntime := regexp.MustCompile(`(?m)(^|\s)python[0-9]*(\s|$)|<<.*PY`)
	scripts, err := filepath.Glob(filepath.Join(root, "scripts", "*.sh"))
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range scripts {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if legacyRuntime.Match(data) {
			t.Fatalf("script still invokes or embeds a Python runtime: %s", path)
		}
	}

	// The workflows are the second place a Python dependency can come back, and
	// the CI job that enforces this same rule only looks at scripts/*.sh -- so a
	// `run: python ...` step added to the workflow itself would slip past both
	// guards. Patterns stay case-sensitive because an invocation is lowercase
	// while the prose naming the rule is not.
	workflowRuntime := regexp.MustCompile(`(?m)actions/setup-python|(^|[^-\w])pip[0-9]*[[:space:]]+install|(^|[[:space:]])python[0-9]*([[:space:]]|$)|<<.*PY`)
	workflows, err := filepath.Glob(filepath.Join(root, ".github", "workflows", "*.y*ml"))
	if err != nil {
		t.Fatal(err)
	}
	if len(workflows) == 0 {
		t.Fatal("no workflow files found, so this guard proves nothing")
	}
	for _, path := range workflows {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		for index, line := range strings.Split(string(data), "\n") {
			// The step that enforces this rule in CI has to spell the runtime it
			// rejects inside its own grep pattern.
			if strings.Contains(line, "git grep") {
				continue
			}
			if workflowRuntime.MatchString(line) {
				t.Errorf("%s:%d reintroduces a Python runtime dependency: %s",
					filepath.ToSlash(path), index+1, strings.TrimSpace(line))
			}
		}
	}
}

func TestScriptLauncherDoesNotTreatGoPackageDirectoryAsExecutable(t *testing.T) {
	data, err := os.ReadFile(filepath.Join("..", "scripts", "lib_playspectra.sh"))
	if err != nil {
		t.Fatal(err)
	}
	launcher := string(data)
	for _, candidate := range []string{"build/playspectra.exe", "build/playspectra"} {
		guard := fmt.Sprintf(`[ -f "$PLAYSPECTRA_SOURCE_ROOT/%s" ] && [ -x "$PLAYSPECTRA_SOURCE_ROOT/%s" ]`, candidate, candidate)
		if !strings.Contains(launcher, guard) {
			t.Errorf("launcher must require %s to be a regular executable file", candidate)
		}
	}
	// The source root holds the playspectra package directory, so resolving a
	// candidate straight off it is what the -f guard above exists to survive.
	// Searching it at all also lets a binary from an older -o playspectra run
	// outrank the current build, so the launcher must not reference it.
	for _, forbidden := range []string{`"$PLAYSPECTRA_SOURCE_ROOT/playspectra.exe"`, `"$PLAYSPECTRA_SOURCE_ROOT/playspectra"`} {
		if strings.Contains(launcher, forbidden) {
			t.Errorf("launcher must not resolve %s from the source root", forbidden)
		}
	}
}

func writeZip(t *testing.T, path string, entries map[string]string) {
	t.Helper()
	file, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	archive := zip.NewWriter(file)
	for name, value := range entries {
		writer, err := archive.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := writer.Write([]byte(value)); err != nil {
			t.Fatal(err)
		}
	}
	if err := archive.Close(); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
}
