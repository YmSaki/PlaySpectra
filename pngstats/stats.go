// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Package pngstats characterizes captured PNG pixels without depending on a
// graphics API or image conversion library.
package pngstats

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
)

var signature = []byte("\x89PNG\r\n\x1a\n")

type Options struct {
	MaxRows    int
	Columns    int
	SampleRows int
}

type colorCount struct {
	key   string
	count int
	order int
}

// Stats returns the same JSON-shaped fields as playspectra_png_stats.py.
// Unsupported, otherwise well-formed encodings retain their geometry and a
// note; malformed compressed pixel data is returned as an error.
func Stats(path string, options Options) (map[string]any, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(data) < len(signature) || !bytes.Equal(data[:8], signature) {
		return map[string]any{"error": "not a PNG", "bytes": len(data)}, nil
	}

	var width, height uint32
	var bitDepth, colorType byte
	idat := []byte{}
	for position := 8; position+8 <= len(data); {
		length := int(binary.BigEndian.Uint32(data[position : position+4]))
		if length < 0 || position+12+length > len(data) {
			return nil, fmt.Errorf("truncated PNG chunk at byte %d", position)
		}
		chunkType := string(data[position+4 : position+8])
		chunk := data[position+8 : position+8+length]
		position += 12 + length
		switch chunkType {
		case "IHDR":
			if len(chunk) < 10 {
				return nil, fmt.Errorf("short IHDR chunk")
			}
			width = binary.BigEndian.Uint32(chunk[0:4])
			height = binary.BigEndian.Uint32(chunk[4:8])
			bitDepth, colorType = chunk[8], chunk[9]
		case "IDAT":
			idat = append(idat, chunk...)
		case "IEND":
			position = len(data)
		}
	}

	info := map[string]any{
		"w": int(width), "h": int(height), "bitdepth": int(bitDepth),
		"colortype": int(colorType), "bytes": len(data),
	}
	channels, supported := map[byte]int{0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colorType]
	if bitDepth != 8 || !supported || len(idat) == 0 {
		info["note"] = "unsupported encoding for pixel stats"
		return info, nil
	}
	if options.Columns <= 0 {
		options.Columns = 200
	}
	if options.SampleRows <= 0 {
		options.SampleRows = 300
	}
	if uint64(width)*uint64(channels) > uint64(math.MaxInt) {
		return nil, fmt.Errorf("PNG row is too wide")
	}
	stride := int(width) * channels
	rows := int(height)
	if options.MaxRows > 0 && options.MaxRows < rows {
		rows = options.MaxRows
	}
	columnStep := max(1, int(width)/max(1, options.Columns))
	rowStep := max(1, rows/max(1, options.SampleRows))
	info["rowsDecoded"] = rows
	info["rowStep"] = rowStep

	reader, err := zlib.NewReader(bytes.NewReader(idat))
	if err != nil {
		return nil, fmt.Errorf("decompress PNG: %w", err)
	}
	raw, readErr := io.ReadAll(reader)
	closeErr := reader.Close()
	if readErr != nil {
		return nil, fmt.Errorf("decompress PNG: %w", readErr)
	}
	if closeErr != nil {
		return nil, fmt.Errorf("decompress PNG: %w", closeErr)
	}

	previous := make([]byte, stride)
	counts := map[string]*colorCount{}
	order := 0
	position := 0
	for y := 0; y < rows; y++ {
		if position >= len(raw) {
			break
		}
		filter := raw[position]
		position++
		if position+stride > len(raw) {
			return nil, fmt.Errorf("truncated PNG row %d", y)
		}
		line := append([]byte(nil), raw[position:position+stride]...)
		position += stride
		unfilter(line, previous, channels, filter)
		if y%rowStep == 0 {
			pixelBytes := min(3, channels)
			for x := 0; x < int(width); x += columnStep {
				start := x * channels
				key := string(line[start : start+pixelBytes])
				entry := counts[key]
				if entry == nil {
					entry = &colorCount{key: key, order: order}
					counts[key] = entry
					order++
				}
				entry.count++
			}
		}
		previous = line
	}

	entries := make([]colorCount, 0, len(counts))
	total := 0
	for _, entry := range counts {
		entries = append(entries, *entry)
		total += entry.count
	}
	if total == 0 {
		total = 1
	}
	sort.SliceStable(entries, func(i, j int) bool {
		if entries[i].count == entries[j].count {
			return entries[i].order < entries[j].order
		}
		return entries[i].count > entries[j].count
	})
	info["distinctColors"] = len(entries)
	dominant := 0.0
	if len(entries) > 0 {
		dominant = float64(entries[0].count) / float64(total)
	}
	info["dominantFraction"] = roundEven(dominant, 4)
	top := make([]any, 0, min(5, len(entries)))
	for _, entry := range entries[:min(5, len(entries))] {
		top = append(top, []any{hex.EncodeToString([]byte(entry.key)), roundEven(float64(entry.count)/float64(total), 3)})
	}
	info["top5"] = top
	return info, nil
}

func unfilter(line, previous []byte, channels int, filter byte) {
	switch filter {
	case 1:
		for i := channels; i < len(line); i++ {
			line[i] += line[i-channels]
		}
	case 2:
		for i := range line {
			line[i] += previous[i]
		}
	case 3:
		for i := range line {
			left := byte(0)
			if i >= channels {
				left = line[i-channels]
			}
			line[i] += byte((int(left) + int(previous[i])) >> 1)
		}
	case 4:
		for i := range line {
			left, upperLeft := byte(0), byte(0)
			if i >= channels {
				left, upperLeft = line[i-channels], previous[i-channels]
			}
			line[i] += paeth(left, previous[i], upperLeft)
		}
	}
}

func paeth(a, b, c byte) byte {
	p := int(a) + int(b) - int(c)
	pa, pb, pc := abs(p-int(a)), abs(p-int(b)), abs(p-int(c))
	if pa <= pb && pa <= pc {
		return a
	}
	if pb <= pc {
		return b
	}
	return c
}

func roundEven(value float64, places int) float64 {
	factor := math.Pow10(places)
	return math.RoundToEven(value*factor) / factor
}

func abs(value int) int {
	if value < 0 {
		return -value
	}
	return value
}

func IsNonDegenerate(stats map[string]any, minColors int, maxDominant float64) bool {
	if minColors == 0 {
		minColors = 3
	}
	if maxDominant == 0 {
		maxDominant = 0.999
	}
	colors, colorsOK := number(stats["distinctColors"])
	dominant, dominantOK := number(stats["dominantFraction"])
	return colorsOK && dominantOK && colors >= float64(minColors) && dominant < maxDominant
}

func number(value any) (float64, bool) {
	switch v := value.(type) {
	case int:
		return float64(v), true
	case float64:
		return v, true
	default:
		return 0, false
	}
}
