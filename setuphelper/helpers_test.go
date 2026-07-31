package setuphelper

import (
	"archive/zip"
	"context"
	"encoding/binary"
	"net"
	"os"
	"path/filepath"
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
	if err := WaitTCP(ctx, "127.0.0.1:1", time.Millisecond); err == nil {
		t.Fatal("closed port did not time out")
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

func TestPatchHelloXRMatchesPythonPatchAndIsIdempotent(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "src", "tests", "hello_xr")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	files := map[string]string{
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
	for name, content := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
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
