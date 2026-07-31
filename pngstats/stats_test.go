package pngstats

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"hash/crc32"
	"os"
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
