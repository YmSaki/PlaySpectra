#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Regenerate the compile_commands.json databases that clangd reads, for every C/C++ tree in the repo:
#   - layer/build-clangd      (MinGW/Ninja)  <- layer/.clangd
#   - driver/build-clangd     (MinGW/Ninja)  <- driver/.clangd
#   - runtime/monado-playspectra/build-clangd (MSVC/Ninja) <- submodule .clangd; also covers devicecore/
#
# WHY a separate tree per build: CMAKE_EXPORT_COMPILE_COMMANDS only works on Ninja/Makefile
# generators, and the canonical builds use generators that cannot emit it (build-win/ is
# "Visual Studio 17 2022", layer/build/ is "MinGW Makefiles", driver/build/ was configured with the
# export OFF). These *-clangd trees are configure-only: they are never built into binaries, so they
# cannot produce a stale artifact that a test might pick up. The canonical build trees are never
# touched by this script.
#
# WHY a database instead of hand-written -I lists in .clangd: most include roots here are
# FetchContent- or vcpkg-provided, so their real paths only exist after configure and move whenever a
# pin changes. With a database, CMakeLists.txt stays the single source of truth.
#
# Run from anywhere:  bash scripts/gen_compile_commands.sh [layer|driver|submodule|all]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WHICH="${1:-all}"
RC=0

# w64devkit supplies the MinGW gcc/g++ that layer/ and driver/ are actually built with; matching the
# real compiler keeps the recorded flags (and therefore clangd's parse) faithful.
#
# Do NOT add clangd's --query-driver for these trees. It makes clangd adopt the true MinGW triple
# (x86_64-w64-windows-gnu) and w64devkit's libstdc++, which then trips a clang-21/libstdc++
# incompatibility -- "no member named 'max_align_t' in the global namespace" on every TU that pulls in
# <cstddef>. Without it clangd falls back to the MSVC-compatible triple and the MSVC/Windows Kits
# system headers, which parse all 57 first-party sources with zero diagnostics. The recorded -I/-D
# flags are honoured either way; only the system-header set differs, and the MSVC one is the one that
# works here.
CC_MINGW="${CC_MINGW:-C:/w64devkit/bin/gcc.exe}"
CXX_MINGW="${CXX_MINGW:-C:/w64devkit/bin/c++.exe}"

step() { printf '\n=== %s ===\n' "$1"; }

# Report on the artifact clangd consumes, not on cmake's exit code: a configure can return 0 after
# skipping generation, and the database is the thing that actually has to exist.
verify_db() {
	if [ -f "$1/compile_commands.json" ]; then
		echo "OK   $1/compile_commands.json ($(grep -c '"file"' "$1/compile_commands.json") entries)"
	else
		echo "FAIL $1/compile_commands.json missing"
		RC=1
	fi
}

gen_layer() {
	step "layer/build-clangd"
	local deps="$REPO/layer/build/_deps"
	local reuse=()
	# Reuse the sources the canonical layer build already fetched, so regenerating needs no network.
	# Falls back to fetching into build-clangd/_deps when that tree is absent (e.g. a fresh clone).
	if [ -d "$deps/openxr_sdk-src" ]; then
		reuse+=(
			"-DFETCHCONTENT_SOURCE_DIR_OPENXR_SDK=$deps/openxr_sdk-src"
			"-DFETCHCONTENT_SOURCE_DIR_LODEPNG=$deps/lodepng-src"
			"-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$deps/nlohmann_json-src"
			"-DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=$deps/vulkan_headers-src"
		)
		[ -d "$deps/googletest-src" ] && reuse+=("-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$deps/googletest-src")
	else
		echo "note: layer/build/_deps not found -- FetchContent will clone into build-clangd/_deps (needs network)"
	fi

	# PLAYSPECTRA_BUILD_TESTS=ON so layer/tests/*.cpp land in the database too; without it clangd has
	# to guess their flags from a neighbouring TU.
	cmake -S "$REPO/layer" -B "$REPO/layer/build-clangd" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_C_COMPILER="$CC_MINGW" \
		-DCMAKE_CXX_COMPILER="$CXX_MINGW" \
		-DPLAYSPECTRA_BUILD_TESTS=ON \
		"${reuse[@]}" >/dev/null || RC=1

	# openxr.h and friends are generated from the OpenXR XML registry at BUILD time. A configure-only
	# tree therefore has the include dir but not the headers, and every layer TU fails with
	# "'openxr/openxr.h' file not found". Generating just these headers costs a few seconds and
	# compiles nothing.
	local inc="_deps/openxr_sdk-build/include/openxr"
	( cd "$REPO/layer/build-clangd" && ninja \
		"$inc/openxr.h" "$inc/openxr_platform.h" "$inc/openxr_reflection.h" \
		"$inc/openxr_loader_negotiation.h" "$inc/openxr_reflection_structs.h" \
		"$inc/openxr_reflection_parent_structs.h" >/dev/null ) || RC=1

	if [ ! -f "$REPO/layer/build-clangd/$inc/openxr.h" ]; then
		echo "FAIL generated openxr.h missing -- layer TUs will not parse"
		RC=1
	fi
	verify_db "$REPO/layer/build-clangd"
}

gen_driver() {
	step "driver/build-clangd"
	local reuse=()
	[ -d "$REPO/driver/build/_deps/nlohmann_json-src" ] &&
		reuse+=("-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$REPO/driver/build/_deps/nlohmann_json-src")
	cmake -S "$REPO/driver" -B "$REPO/driver/build-clangd" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_C_COMPILER="$CC_MINGW" \
		-DCMAKE_CXX_COMPILER="$CXX_MINGW" \
		"${reuse[@]}" >/dev/null || RC=1
	verify_db "$REPO/driver/build-clangd"
}

gen_submodule() {
	step "runtime/monado-playspectra/build-clangd"
	local src="$REPO/runtime/monado-playspectra"
	local bld="$src/build-clangd"
	if [ ! -f "$src/CMakeLists.txt" ]; then
		echo "SKIP submodule not checked out (git submodule update --init runtime/monado-playspectra)"
		return
	fi
	export VULKAN_SDK="${VULKAN_SDK:-C:/VulkanSDK/1.4.350.0}"

	if [ -f "$bld/CMakeCache.txt" ]; then
		# Incremental reconfigure: the cache already holds the MSVC paths and the vcpkg manifest is
		# already installed under build-clangd/vcpkg_installed, so this is seconds, not the 20-40
		# minutes a first vcpkg run costs.
		cmake -S "$src" -B "$bld" >/dev/null || RC=1
	else
		local tc="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
		echo "note: first configure -- vcpkg will install the manifest (can take 20-40 min)"
		cmake -S "$src" -B "$bld" -G Ninja \
			-DCMAKE_TOOLCHAIN_FILE="$tc" \
			-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON \
			-DXRT_BUILD_DRIVER_PLAYSPECTRA=ON \
			-DXRT_MODULE_MONADO_CLI=ON \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null || RC=1
	fi
	verify_db "$bld"

	# devicecore/*.c are compiled into drv_playspectra, so this database is also what resolves them
	# (see devicecore/.clangd). If the split ever stops being reflected here, clangd silently loses
	# the devicecore include path -- assert it instead.
	if [ -f "$bld/compile_commands.json" ] &&
		! grep -q 'devicecore' "$bld/compile_commands.json"; then
		echo "FAIL database has no devicecore entries -- devicecore/ will not resolve"
		RC=1
	fi
}

case "$WHICH" in
layer) gen_layer ;;
driver) gen_driver ;;
submodule) gen_submodule ;;
all)
	gen_layer
	gen_driver
	gen_submodule
	;;
*)
	echo "usage: bash scripts/gen_compile_commands.sh [layer|driver|submodule|all]"
	exit 2
	;;
esac

step "result"
[ "$RC" -eq 0 ] && echo "ALL GREEN" || echo "FAILURES -- see above"
exit "$RC"
