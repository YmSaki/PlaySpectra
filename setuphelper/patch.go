// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package setuphelper

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type replacement struct {
	marker string
	old    string
	new    string
}

func PatchHelloXR(sourceRoot string) ([]string, error) {
	paths := []struct {
		path         string
		replacements []replacement
	}{
		{filepath.Join(sourceRoot, "src", "tests", "hello_xr", "logger.cpp"), []replacement{{
			marker: "#include <chrono>", old: "#include <sstream>", new: "#include <chrono>\n#include <sstream>",
		}}},
		{filepath.Join(sourceRoot, "src", "tests", "hello_xr", "graphicsplugin_d3d11.cpp"), d3d11Replacements()},
		{filepath.Join(sourceRoot, "src", "tests", "hello_xr", "graphicsplugin_d3d12.cpp"), d3d12Replacements()},
	}
	var changed []string
	for _, file := range paths {
		markers, err := patchFile(file.path, file.replacements)
		if err != nil {
			return changed, err
		}
		changed = append(changed, markers...)
	}
	return changed, nil
}

func patchFile(path string, replacements []replacement) ([]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	text := strings.ReplaceAll(string(data), "\r\n", "\n")
	changed := []string{}
	for _, item := range replacements {
		if strings.Contains(text, item.marker) {
			continue
		}
		if count := strings.Count(text, item.old); count != 1 {
			return changed, fmt.Errorf("%s: anchor for %q occurs %d times", path, item.marker, count)
		}
		text = strings.Replace(text, item.old, item.new, 1)
		changed = append(changed, item.marker)
	}
	if len(changed) > 0 {
		info, err := os.Stat(path)
		if err != nil {
			return changed, err
		}
		if err := os.WriteFile(path, []byte(text), info.Mode().Perm()); err != nil {
			return changed, err
		}
	}
	return changed, nil
}

func d3d11Replacements() []replacement {
	return []replacement{
		{
			marker: "HELLO_XR_SAMPLE_COUNT",
			old:    `uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override { return 1; }`,
			new: `uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override {
        const char* e = std::getenv("HELLO_XR_SAMPLE_COUNT");
        const int n = e ? std::atoi(e) : 1;
        return n > 1 ? static_cast<uint32_t>(n) : 1u;
    }`,
		},
		{marker: "#include <cstdlib>", old: `#include "pch.h"`, new: "#include \"pch.h\"\n\n#include <cstdlib>"},
		{
			marker: "rtvColorDesc",
			old:    `const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, (DXGI_FORMAT)swapchainFormat);`,
			new: `D3D11_TEXTURE2D_DESC rtvColorDesc;
        colorTexture->GetDesc(&rtvColorDesc);
        const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(
            rtvColorDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D,
            (DXGI_FORMAT)swapchainFormat);`,
		},
		{marker: "colorDesc.SampleDesc.Count;", old: `depthDesc.SampleDesc.Count = 1;`, new: `depthDesc.SampleDesc.Count = colorDesc.SampleDesc.Count;`},
		{
			marker: "D3D11_DSV_DIMENSION_TEXTURE2DMS",
			old:    `CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);`,
			new: `CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(
            colorDesc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D,
            DXGI_FORMAT_D32_FLOAT);`,
		},
		{
			marker: "HELLO_XR_HDR",
			old: "int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n" +
				"        // List of supported color swapchain formats.",
			new: `int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        const char* hdrEnv = std::getenv("HELLO_XR_HDR");
        if (hdrEnv && *hdrEnv && *hdrEnv != '0') {
            for (int64_t f : runtimeFormats) {
                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return f;
            }
        }
        // List of supported color swapchain formats.`,
		},
		{
			marker: "HELLO_XR_TYPELESS",
			old: "        }\n" +
				"        // List of supported color swapchain formats.",
			new: `        }
        const char* typelessEnv = std::getenv("HELLO_XR_TYPELESS");
        if (typelessEnv && *typelessEnv && *typelessEnv != '0') {
            for (int64_t f : runtimeFormats) {
                if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) return f;
            }
        }
        // List of supported color swapchain formats.`,
		},
		{
			marker: "R8G8B8A8_TYPELESS   ? DXGI_FORMAT",
			old:    `            (DXGI_FORMAT)swapchainFormat);`,
			new: `            swapchainFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
            : swapchainFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
            : (DXGI_FORMAT)swapchainFormat);`,
		},
	}
}

func d3d12Replacements() []replacement {
	return []replacement{
		{
			marker: "HELLO_XR_SAMPLE_COUNT",
			old:    `    void UpdateOptions(const std::shared_ptr<Options>& options) override { m_clearColor = options->GetBackgroundClearColor(); }`,
			new: `    uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override {
        const char* e = std::getenv("HELLO_XR_SAMPLE_COUNT");
        const int n = e ? std::atoi(e) : 1;
        return n > 1 ? static_cast<uint32_t>(n) : 1u;
    }

    void UpdateOptions(const std::shared_ptr<Options>& options) override { m_clearColor = options->GetBackgroundClearColor(); }`,
		},
		{marker: "#include <cstdlib>", old: `#include "pch.h"`, new: "#include \"pch.h\"\n\n#include <cstdlib>"},
		{marker: "colorDesc.SampleDesc.Count;", old: `depthDesc.SampleDesc.Count = 1;`, new: `depthDesc.SampleDesc.Count = colorDesc.SampleDesc.Count;`},
		{
			marker: "psoSampleCountEnv",
			old:    `pipelineStateDesc.SampleDesc = {1, 0};`,
			new: `{
            const char* psoSampleCountEnv = std::getenv("HELLO_XR_SAMPLE_COUNT");
            const int n = psoSampleCountEnv ? std::atoi(psoSampleCountEnv) : 1;
            pipelineStateDesc.SampleDesc = {n > 1 ? static_cast<UINT>(n) : 1u, 0};
        }`,
		},
		{
			marker: "HELLO_XR_HDR",
			old: "int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n" +
				"        // List of supported color swapchain formats.",
			new: `int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        const char* hdrEnv = std::getenv("HELLO_XR_HDR");
        if (hdrEnv && *hdrEnv && *hdrEnv != '0') {
            for (int64_t f : runtimeFormats) {
                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return f;
            }
        }
        // List of supported color swapchain formats.`,
		},
	}
}
