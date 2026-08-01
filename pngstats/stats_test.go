package pngstats

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"hash/crc32"
	"os"
	"strings"
	"testing"
)

func pngChunk(kind string, data []byte) []byte {
	var out bytes.Buffer
	_ = binary.Write(&out, binary.BigEndian, uint32(len(data)))
	out.WriteString(kind)
	out.Write(data)
	_ = binary.Write(&out, binary.BigEndian, crc32.ChecksumIEEE(append([]byte(kind), data...)))
	return out.Bytes()
}

func makePNG(t *testing.T, width, height int, bitDepth, colorType byte, rows [][]byte, filters []byte) []byte {
	t.Helper()
	ihdr := make([]byte, 13)
	binary.BigEndian.PutUint32(ihdr[0:4], uint32(width))
	binary.BigEndian.PutUint32(ihdr[4:8], uint32(height))
	ihdr[8], ihdr[9] = bitDepth, colorType
	var filtered bytes.Buffer
	channels := map[byte]int{0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colorType]
	previous := make([]byte, width*channels)
	for index, row := range rows {
		filter := filters[index%len(filters)]
		filtered.WriteByte(filter)
		filtered.Write(encodeFilter(row, previous, channels, filter))
		previous = append(previous[:0], row...)
	}
	var compressed bytes.Buffer
	zw := zlib.NewWriter(&compressed)
	_, _ = zw.Write(filtered.Bytes())
	_ = zw.Close()
	return bytes.Join([][]byte{signature, pngChunk("IHDR", ihdr), pngChunk("IDAT", compressed.Bytes()), pngChunk("IEND", nil)}, nil)
}

func encodeFilter(row, previous []byte, channels int, filter byte) []byte {
	out := make([]byte, len(row))
	for i, value := range row {
		left, upperLeft := byte(0), byte(0)
		if i >= channels {
			left, upperLeft = row[i-channels], previous[i-channels]
		}
		predictor := byte(0)
		switch filter {
		case 1:
			predictor = left
		case 2:
			predictor = previous[i]
		case 3:
			predictor = byte((int(left) + int(previous[i])) >> 1)
		case 4:
			predictor = paeth(left, previous[i], upperLeft)
		}
		out[i] = value - predictor
	}
	return out
}

func writeFixture(t *testing.T, data []byte) string {
	t.Helper()
	path := t.TempDir() + "/fixture.png"
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestAllPNGFiltersDecodeToSameColors(t *testing.T) {
	row := []byte{255, 0, 0, 0, 255, 0, 0, 0, 255}
	rows := [][]byte{row, row, row, row, row}
	path := writeFixture(t, makePNG(t, 3, 5, 8, 2, rows, []byte{0, 1, 2, 3, 4}))
	stats, err := Stats(path, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if stats["distinctColors"] != 3 || stats["dominantFraction"] != 0.3333 || stats["rowsDecoded"] != 5 || stats["rowStep"] != 1 {
		t.Fatalf("stats = %v", stats)
	}
	top := stats["top5"].([]any)
	if top[0].([]any)[0] != "ff0000" || top[1].([]any)[0] != "00ff00" || top[2].([]any)[0] != "0000ff" {
		t.Fatalf("top5 = %v", top)
	}
	if !IsNonDegenerate(stats, 0, 0) {
		t.Fatalf("expected non-degenerate: %v", stats)
	}
}

func TestAllSupportedColorTypesUsePythonPixelKeyWidth(t *testing.T) {
	tests := []struct {
		name      string
		colorType byte
		row       []byte
		firstKey  string
	}{
		{"grayscale", 0, []byte{1, 2, 3}, "01"},
		{"rgb", 2, []byte{1, 2, 3, 4, 5, 6, 7, 8, 9}, "010203"},
		{"palette index", 3, []byte{1, 2, 3}, "01"},
		{"grayscale alpha", 4, []byte{1, 255, 2, 128, 3, 0}, "01ff"},
		{"rgba ignores alpha", 6, []byte{1, 2, 3, 255, 4, 5, 6, 128, 7, 8, 9, 0}, "010203"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			path := writeFixture(t, makePNG(t, 3, 1, 8, test.colorType, [][]byte{test.row}, []byte{0}))
			stats, err := Stats(path, Options{})
			if err != nil || stats["distinctColors"] != 3 {
				t.Fatalf("stats=%v err=%v", stats, err)
			}
			if got := stats["top5"].([]any)[0].([]any)[0]; got != test.firstKey {
				t.Fatalf("first key = %v, want %s", got, test.firstKey)
			}
		})
	}
}

func TestSamplingSpansWholeHeightAndHonorsMaxRows(t *testing.T) {
	rows := make([][]byte, 600)
	for y := range rows {
		value := byte(0)
		if y >= 300 {
			value = byte(1 + y%3)
		}
		rows[y] = []byte{value, value, value}
	}
	path := writeFixture(t, makePNG(t, 1, 600, 8, 2, rows, []byte{0}))
	stats, err := Stats(path, Options{SampleRows: 300})
	if err != nil || stats["rowStep"] != 2 || stats["distinctColors"].(int) < 4 {
		t.Fatalf("whole-height stats=%v err=%v", stats, err)
	}
	topOnly, err := Stats(path, Options{MaxRows: 300, SampleRows: 300})
	if err != nil || topOnly["distinctColors"] != 1 || topOnly["rowsDecoded"] != 300 {
		t.Fatalf("max-row stats=%v err=%v", topOnly, err)
	}
}

func TestInvalidAndUnsupportedPNGResults(t *testing.T) {
	path := writeFixture(t, []byte("not png"))
	stats, err := Stats(path, Options{})
	if err != nil || stats["error"] != "not a PNG" || stats["bytes"] != 7 {
		t.Fatalf("stats=%v err=%v", stats, err)
	}
	unsupported := writeFixture(t, makePNG(t, 2, 1, 16, 2, [][]byte{{0, 0, 0, 0, 0, 0}}, []byte{0}))
	stats, err = Stats(unsupported, Options{})
	if err != nil || stats["note"] != "unsupported encoding for pixel stats" || stats["w"] != 2 || stats["h"] != 1 {
		t.Fatalf("stats=%v err=%v", stats, err)
	}
}

// makePNGFromRawStream assembles a PNG around an already-filtered pixel stream
// so a test can declare a height the stream does not actually cover -- the
// shape a capture read while it is still being written takes.
func makePNGFromRawStream(t *testing.T, width, height int, colorType byte, raw []byte) []byte {
	t.Helper()
	ihdr := make([]byte, 13)
	binary.BigEndian.PutUint32(ihdr[0:4], uint32(width))
	binary.BigEndian.PutUint32(ihdr[4:8], uint32(height))
	ihdr[8], ihdr[9] = 8, colorType
	var compressed bytes.Buffer
	writer := zlib.NewWriter(&compressed)
	if _, err := writer.Write(raw); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return bytes.Join([][]byte{signature, pngChunk("IHDR", ihdr), pngChunk("IDAT", compressed.Bytes()), pngChunk("IEND", nil)}, nil)
}

// TestTruncatedPNGResults covers reading a capture that is still being written.
// The suite decides "the frame is a rendered scene, not a flat fill" from these
// stats, so a half-written file that decodes to a plausible-looking result is
// worse than one that errors: it turns a read race into a claim about what the
// app rendered.
func TestTruncatedPNGResults(t *testing.T) {
	// Three RGB pixels per row, so each row costs one filter byte plus nine.
	rows, stride := 4, 9
	full := make([]byte, 0, rows*(stride+1))
	for y := range rows {
		full = append(full, 0)
		for x := range stride {
			full = append(full, byte(y*stride+x))
		}
	}

	t.Run("a row cut in half is an error", func(t *testing.T) {
		path := writeFixture(t, makePNGFromRawStream(t, 3, rows, 2, full[:2*(stride+1)+1+stride/2]))
		stats, err := Stats(path, Options{})
		if err == nil {
			t.Fatalf("a half-written row decoded to %v", stats)
		}
		if !strings.Contains(err.Error(), "truncated PNG row") {
			t.Fatalf("err = %v", err)
		}
	})

	// Characterization: a stream that stops exactly on a row boundary is not
	// detectable from the pixel data alone, so the rows that did arrive are
	// reported. rowsDecoded stays the declared height -- it is the number of
	// rows the sampler intended to read, not the number it got.
	t.Run("a stream cut on a row boundary reports what arrived", func(t *testing.T) {
		complete, err := Stats(writeFixture(t, makePNGFromRawStream(t, 3, rows, 2, full)), Options{})
		if err != nil {
			t.Fatal(err)
		}
		// Every pixel in the fixture differs, so the colour count is three per
		// row that made it into the stream.
		if complete["distinctColors"] != 3*rows {
			t.Fatalf("complete stats = %v", complete)
		}
		path := writeFixture(t, makePNGFromRawStream(t, 3, rows, 2, full[:2*(stride+1)]))
		stats, err := Stats(path, Options{})
		if err != nil {
			t.Fatal(err)
		}
		if stats["distinctColors"] != 6 || stats["rowsDecoded"] != rows {
			t.Fatalf("stats = %v", stats)
		}
	})

	t.Run("a chunk cut short is an error", func(t *testing.T) {
		row := []byte{255, 0, 0, 0, 255, 0, 0, 0, 255}
		complete := makePNG(t, 3, 5, 8, 2, [][]byte{row, row, row, row, row}, []byte{0})
		if len(complete) <= 53 {
			t.Fatalf("fixture is too small to cut inside its IDAT: %d bytes", len(complete))
		}
		path := writeFixture(t, complete[:53])
		stats, err := Stats(path, Options{})
		if err == nil {
			t.Fatalf("a truncated chunk decoded to %v", stats)
		}
		if !strings.Contains(err.Error(), "truncated PNG chunk") {
			t.Fatalf("err = %v", err)
		}
	})

	t.Run("unreadable compressed data is an error", func(t *testing.T) {
		ihdr := make([]byte, 13)
		binary.BigEndian.PutUint32(ihdr[0:4], 3)
		binary.BigEndian.PutUint32(ihdr[4:8], uint32(rows))
		ihdr[8], ihdr[9] = 8, 2
		garbage := bytes.Join([][]byte{signature, pngChunk("IHDR", ihdr), pngChunk("IDAT", []byte("not zlib")), pngChunk("IEND", nil)}, nil)
		path := writeFixture(t, garbage)
		stats, err := Stats(path, Options{})
		if err == nil {
			t.Fatalf("undecompressable pixel data decoded to %v", stats)
		}
		if !strings.Contains(err.Error(), "decompress PNG") {
			t.Fatalf("err = %v", err)
		}
	})

	// Characterization: a file cut before its IHDR still carries the PNG
	// signature, so it is reported as a well-formed image of unsupported
	// encoding rather than as a truncation. Callers separate this from a real
	// image by the absence of pixel stats, not by an error.
	t.Run("a file cut before its header is reported as unsupported", func(t *testing.T) {
		path := writeFixture(t, signature[:8])
		stats, err := Stats(path, Options{})
		if err != nil {
			t.Fatal(err)
		}
		if stats["note"] != "unsupported encoding for pixel stats" || stats["w"] != 0 || stats["h"] != 0 {
			t.Fatalf("stats = %v", stats)
		}
		if IsNonDegenerate(stats, 0, 0) {
			t.Fatalf("a header-less file was accepted as a rendered frame: %v", stats)
		}
	})
}

func TestDegenerateThresholds(t *testing.T) {
	if IsNonDegenerate(map[string]any{"distinctColors": 2, "dominantFraction": 0.5}, 0, 0) {
		t.Fatal("two colors should be degenerate")
	}
	if IsNonDegenerate(map[string]any{"distinctColors": 3, "dominantFraction": 0.999}, 0, 0) {
		t.Fatal("dominant threshold is strict")
	}
	if !IsNonDegenerate(map[string]any{"distinctColors": 3, "dominantFraction": 0.9989}, 0, 0) {
		t.Fatal("valid rendered sample rejected")
	}
}

// TestDegenerateThresholdsAreHonored exercises the two arguments with values
// other than the zero that selects the defaults. Every existing caller passes
// (0, 0), so a version that ignored its arguments entirely would pass the whole
// suite -- and a caller tightening the bar for a specific capture would get the
// default answer without knowing it.
func TestDegenerateThresholdsAreHonored(t *testing.T) {
	sample := map[string]any{"distinctColors": 4, "dominantFraction": 0.5}
	tests := []struct {
		name        string
		stats       map[string]any
		minColors   int
		maxDominant float64
		want        bool
	}{
		{"four colors clear the default bar", sample, 0, 0, true},
		{"but not a demand for ten", sample, 10, 0, false},
		{"exactly the demanded count passes", sample, 4, 0, true},
		{"one more than there are fails", sample, 5, 0, false},
		{"a dominant half fails a 0.4 ceiling", sample, 0, 0.4, false},
		{"and clears a 0.6 one", sample, 0, 0.6, true},
		{"the ceiling is exclusive", sample, 0, 0.5, false},
		{"two colors pass when two is the bar", map[string]any{"distinctColors": 2, "dominantFraction": 0.5}, 2, 0, true},
		{"missing stats are not a rendered frame", nil, 0, 0, false},
		{"an error result is not a rendered frame", map[string]any{"error": "not a PNG", "bytes": 7}, 0, 0, false},
		{"a partial result is not a rendered frame", map[string]any{"distinctColors": 4}, 0, 0, false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := IsNonDegenerate(test.stats, test.minColors, test.maxDominant); got != test.want {
				t.Fatalf("IsNonDegenerate(%v, %d, %v) = %v, want %v", test.stats, test.minColors, test.maxDominant, got, test.want)
			}
		})
	}
}
