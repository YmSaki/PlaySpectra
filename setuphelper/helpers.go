package setuphelper

import (
	"archive/zip"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

func TransformVCXProj(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	text := strings.TrimPrefix(string(data), "\ufeff")
	text = strings.ReplaceAll(text, "Win32Proj", "__W32PROJ__")
	text = strings.ReplaceAll(text, "Win32", "x64")
	text = strings.ReplaceAll(text, "__W32PROJ__", "Win32Proj")
	text = strings.ReplaceAll(text, "win32", "win64")
	out := strings.TrimSuffix(path, filepath.Ext(path)) + "_x64" + filepath.Ext(path)
	if err := os.WriteFile(out, []byte(text), 0o644); err != nil {
		return "", err
	}
	return out, nil
}

func DeployHelloVR(samples, destination, root string) ([]string, error) {
	win64 := filepath.Join(destination, "win64")
	if err := os.MkdirAll(win64, 0o755); err != nil {
		return nil, err
	}
	for _, name := range []string{"cube_texture.png", "hellovr_actions.json", "hellovr_bindings_generic.json", "hellovr_bindings_vive_controller.json"} {
		if err := copyFile(filepath.Join(samples, "bin", name), filepath.Join(destination, name)); err != nil {
			return nil, err
		}
	}
	if err := copyTree(filepath.Join(samples, "bin", "shaders"), filepath.Join(destination, "shaders")); err != nil {
		return nil, err
	}
	for source, target := range map[string]string{
		filepath.Join(samples, "bin", "win64", "hellovr_dx12_x64.exe"):        filepath.Join(win64, "hellovr_dx12.exe"),
		filepath.Join(root, "third_party", "monado", "bin", "SDL2.dll"):       filepath.Join(win64, "SDL2.dll"),
		filepath.Join(root, "third_party", "opencomposite", "openvr_api.dll"): filepath.Join(win64, "openvr_api.dll"),
	} {
		if err := copyFile(source, target); err != nil {
			return nil, err
		}
	}
	entries, err := os.ReadDir(win64)
	if err != nil {
		return nil, err
	}
	names := make([]string, 0, len(entries))
	for _, entry := range entries {
		names = append(names, entry.Name())
	}
	sort.Strings(names)
	return names, nil
}

func ExtractMonado(zipPath, destination string) (int, error) {
	archive, err := zip.OpenReader(zipPath)
	if err != nil {
		return 0, err
	}
	defer archive.Close()
	root, err := filepath.Abs(destination)
	if err != nil {
		return 0, err
	}
	count := 0
	for _, entry := range archive.File {
		name := strings.ReplaceAll(entry.Name, "\\", "/")
		if entry.FileInfo().IsDir() || !strings.HasPrefix(name, "install/") || strings.HasPrefix(name, "install/share/steamvr-monado") {
			continue
		}
		rel := strings.TrimPrefix(name, "install/")
		target := filepath.Join(root, filepath.FromSlash(rel))
		absolute, err := filepath.Abs(target)
		if err != nil || (absolute != root && !strings.HasPrefix(absolute, root+string(os.PathSeparator))) {
			return count, fmt.Errorf("unsafe zip path %q", entry.Name)
		}
		if err := os.MkdirAll(filepath.Dir(absolute), 0o755); err != nil {
			return count, err
		}
		input, err := entry.Open()
		if err != nil {
			return count, err
		}
		mode := entry.Mode().Perm()
		if mode == 0 {
			mode = 0o644
		}
		output, err := os.OpenFile(absolute, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, mode)
		if err == nil {
			_, err = io.Copy(output, input)
		}
		closeOutputErr := error(nil)
		if output != nil {
			closeOutputErr = output.Close()
		}
		closeInputErr := input.Close()
		if err != nil {
			return count, err
		}
		if closeOutputErr != nil {
			return count, closeOutputErr
		}
		if closeInputErr != nil {
			return count, closeInputErr
		}
		count++
	}
	// A Monado CI archive always carries an install/ tree. Nothing there means
	// the artifact layout changed, not that there was nothing to do.
	if count == 0 {
		return 0, fmt.Errorf("%s: extracted 0 files, the archive has no install/ tree", zipPath)
	}
	return count, nil
}

func CheckPEX64(path string) (int64, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	size := int64(len(data))
	if len(data) <= 0x1000 || len(data) < 0x40 || string(data[:2]) != "MZ" {
		return size, errors.New("not a PE32+ x64 DLL")
	}
	offset := int(binary.LittleEndian.Uint32(data[0x3c:0x40]))
	if offset < 0 || offset+6 > len(data) || string(data[offset:offset+4]) != "PE\x00\x00" || binary.LittleEndian.Uint16(data[offset+4:offset+6]) != 0x8664 {
		return size, errors.New("not a PE32+ x64 DLL")
	}
	return size, nil
}

func WaitTCP(ctx context.Context, address string, interval time.Duration) error {
	if interval <= 0 {
		interval = 300 * time.Millisecond
	}
	var last error
	for {
		connection, err := (&net.Dialer{Timeout: interval}).DialContext(ctx, "tcp", address)
		if err == nil {
			_ = connection.Close()
			return nil
		}
		last = err
		select {
		case <-ctx.Done():
			return fmt.Errorf("wait for %s: %w (last dial: %v)", address, ctx.Err(), last)
		case <-time.After(interval):
		}
	}
}

type CaptureComparison struct {
	Frames       int    `json:"frames"`
	Distinct     int    `json:"distinct_contents"`
	BaselineHash string `json:"baseline_hash,omitempty"`
	PostHash     string `json:"post_hash,omitempty"`
	OK           bool   `json:"ok"`
}

func CompareCaptures(recordingDir, baselinePath, postPath string) (CaptureComparison, error) {
	files, err := filepath.Glob(filepath.Join(recordingDir, "rec_*.png"))
	if err != nil {
		return CaptureComparison{}, err
	}
	sort.Strings(files)
	distinct := map[string]struct{}{}
	for _, path := range files {
		hash, err := shortHash(path)
		if err != nil {
			return CaptureComparison{}, err
		}
		distinct[hash] = struct{}{}
	}
	result := CaptureComparison{Frames: len(files), Distinct: len(distinct)}
	if baselinePath != "" {
		result.BaselineHash, err = shortHash(baselinePath)
		if err != nil {
			return CaptureComparison{}, err
		}
	}
	if postPath != "" {
		result.PostHash, err = shortHash(postPath)
		if err != nil {
			return CaptureComparison{}, err
		}
	}
	result.OK = result.Distinct >= 2 && result.BaselineHash != "" && result.PostHash != "" && result.BaselineHash != result.PostHash
	return result, nil
}

func shortHash(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", sha256.Sum256(data))[:12], nil
}

func copyTree(source, target string) error {
	return filepath.WalkDir(source, func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		destination := filepath.Join(target, relative)
		if entry.IsDir() {
			return os.MkdirAll(destination, 0o755)
		}
		return copyFile(path, destination)
	})
}

func copyFile(source, target string) error {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	info, err := input.Stat()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	output, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, info.Mode().Perm())
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(output, input)
	closeErr := output.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}
