// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Package protocol implements the PlaySpectra NDJSON/TCP wire protocol.
package protocol

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

const (
	ProtocolVersion = 1
	MaxMessageSize  = 1 << 20
	DefaultTimeout  = 5 * time.Second
)

// Client is a serialized request/response client for an adapter or layer
// control channel. The adapter may send asynchronous event objects; Request
// ignores those until it sees the requested request_id.
type Client struct {
	conn   net.Conn
	read   *bufio.Reader
	mu     sync.Mutex
	events []map[string]any
}

func Dial(ctx context.Context, address string) (*Client, error) {
	if _, ok := ctx.Deadline(); !ok {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, DefaultTimeout)
		defer cancel()
	}
	var d net.Dialer
	conn, err := d.DialContext(ctx, "tcp", address)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", address, err)
	}
	return NewClient(conn), nil
}

func NewClient(conn net.Conn) *Client {
	return &Client{conn: conn, read: bufio.NewReaderSize(conn, 32*1024)}
}

func (c *Client) Request(ctx context.Context, req map[string]any) (map[string]any, error) {
	ctx, cancel := withDefaultTimeout(ctx)
	defer cancel()
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.write(ctx, req); err != nil {
		return nil, err
	}
	want, _ := req["request_id"].(string)
	for {
		obj, err := c.readObject(ctx)
		if err != nil {
			return nil, err
		}
		if _, event := obj["event"]; event {
			c.events = append(c.events, obj)
			continue
		}
		if want == "" {
			return obj, nil
		}
		if got, _ := obj["request_id"].(string); got == want {
			return obj, nil
		}
	}
}

// RequestLine is used by the capture channel, whose historical protocol is
// one request followed by one response without request_id echoing.
func (c *Client) RequestLine(ctx context.Context, req map[string]any) (map[string]any, error) {
	ctx, cancel := withDefaultTimeout(ctx)
	defer cancel()
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.write(ctx, req); err != nil {
		return nil, err
	}
	for {
		obj, err := c.readObject(ctx)
		if err != nil {
			return nil, err
		}
		if _, event := obj["event"]; !event {
			return obj, nil
		}
		c.events = append(c.events, obj)
	}
}

// WaitEvent returns a named asynchronous event. Events encountered while a
// request waits for its matching response are queued instead of discarded.
func (c *Client) WaitEvent(ctx context.Context, name string) (map[string]any, error) {
	ctx, cancel := withDefaultTimeout(ctx)
	defer cancel()
	c.mu.Lock()
	defer c.mu.Unlock()
	for index, event := range c.events {
		if name == "" || event["event"] == name {
			c.events = append(c.events[:index], c.events[index+1:]...)
			return event, nil
		}
	}
	for {
		object, err := c.readObject(ctx)
		if err != nil {
			return nil, err
		}
		if _, event := object["event"]; !event {
			continue
		}
		if name == "" || object["event"] == name {
			return object, nil
		}
		c.events = append(c.events, object)
	}
}

// SendOnly sends a set_state frame without waiting for its reply. Replies are
// consumed by a later Request, just like the Python reference client.
func (c *Client) SendOnly(ctx context.Context, req map[string]any) error {
	ctx, cancel := withDefaultTimeout(ctx)
	defer cancel()
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.write(ctx, req)
}

func (c *Client) write(ctx context.Context, req map[string]any) error {
	b, err := json.Marshal(req)
	if err != nil {
		return fmt.Errorf("encode request: %w", err)
	}
	if len(b)+1 > MaxMessageSize {
		return fmt.Errorf("request exceeds %d byte limit", MaxMessageSize)
	}
	if err := setDeadline(c.conn, ctx); err != nil {
		return err
	}
	b = append(b, '\n')
	for len(b) > 0 {
		n, err := c.conn.Write(b)
		if err != nil {
			return fmt.Errorf("write NDJSON request: %w", err)
		}
		b = b[n:]
	}
	return nil
}

func (c *Client) readObject(ctx context.Context) (map[string]any, error) {
	if err := setDeadline(c.conn, ctx); err != nil {
		return nil, err
	}
	line, err := c.read.ReadString('\n')
	if err != nil {
		if errors.Is(err, io.EOF) {
			return nil, io.ErrUnexpectedEOF
		}
		return nil, fmt.Errorf("read NDJSON response: %w", err)
	}
	if len(line) > MaxMessageSize {
		return nil, fmt.Errorf("response exceeds %d byte limit", MaxMessageSize)
	}
	line = strings.TrimSpace(line)
	if line == "" {
		return c.readObject(ctx)
	}
	var obj map[string]any
	if err := json.Unmarshal([]byte(line), &obj); err != nil {
		return nil, fmt.Errorf("decode NDJSON response: %w", err)
	}
	return obj, nil
}

func setDeadline(conn net.Conn, ctx context.Context) error {
	if deadline, ok := ctx.Deadline(); ok {
		return conn.SetDeadline(deadline)
	}
	return conn.SetDeadline(time.Time{})
}

func withDefaultTimeout(ctx context.Context) (context.Context, context.CancelFunc) {
	if ctx == nil {
		ctx = context.Background()
	}
	if _, ok := ctx.Deadline(); ok {
		return ctx, func() {}
	}
	return context.WithTimeout(ctx, DefaultTimeout)
}

func (c *Client) Close() error {
	if c == nil || c.conn == nil {
		return nil
	}
	return c.conn.Close()
}
