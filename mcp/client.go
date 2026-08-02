// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

package mcp

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
)

type Client struct {
	output  io.Writer
	write   sync.Mutex
	mu      sync.Mutex
	nextID  int64
	pending map[string]chan clientResponse
	done    chan struct{}
	err     error
}

type clientResponse struct {
	message map[string]any
	err     error
}

func NewClient(input io.Reader, output io.Writer) *Client {
	client := &Client{output: output, pending: map[string]chan clientResponse{}, done: make(chan struct{})}
	go client.readLoop(input)
	return client
}

func (c *Client) Request(ctx context.Context, method string, params map[string]any) (map[string]any, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	c.mu.Lock()
	c.nextID++
	id := c.nextID
	key := fmt.Sprint(id)
	response := make(chan clientResponse, 1)
	c.pending[key] = response
	c.mu.Unlock()
	request := map[string]any{"jsonrpc": "2.0", "id": id, "method": method}
	if params != nil {
		request["params"] = params
	}
	if err := c.writeMessage(request); err != nil {
		c.removePending(key)
		return nil, err
	}
	select {
	case result := <-response:
		return result.message, result.err
	case <-ctx.Done():
		c.removePending(key)
		return nil, ctx.Err()
	case <-c.done:
		c.mu.Lock()
		err := c.err
		c.mu.Unlock()
		if err == nil {
			err = io.EOF
		}
		return nil, err
	}
}

func (c *Client) Notify(method string, params map[string]any) error {
	message := map[string]any{"jsonrpc": "2.0", "method": method}
	if params != nil {
		message["params"] = params
	}
	return c.writeMessage(message)
}

func (c *Client) Initialize(ctx context.Context) (map[string]any, error) {
	response, err := c.Request(ctx, "initialize", map[string]any{
		"protocolVersion": "2024-11-05", "capabilities": map[string]any{},
		"clientInfo": map[string]any{"name": "playspectra-verifier", "version": "0.1.0"},
	})
	if err != nil {
		return nil, err
	}
	if response["error"] != nil {
		return nil, fmt.Errorf("initialize: %v", response["error"])
	}
	if err := c.Notify("notifications/initialized", nil); err != nil {
		return nil, err
	}
	result, _ := response["result"].(map[string]any)
	return result, nil
}

func (c *Client) ListTools(ctx context.Context) ([]map[string]any, error) {
	response, err := c.Request(ctx, "tools/list", map[string]any{})
	if err != nil {
		return nil, err
	}
	if response["error"] != nil {
		return nil, fmt.Errorf("tools/list: %v", response["error"])
	}
	result, _ := response["result"].(map[string]any)
	rawTools, _ := result["tools"].([]any)
	tools := make([]map[string]any, 0, len(rawTools))
	for _, raw := range rawTools {
		if tool, ok := raw.(map[string]any); ok {
			tools = append(tools, tool)
		}
	}
	return tools, nil
}

func (c *Client) CallTool(ctx context.Context, name string, arguments map[string]any) (map[string]any, error) {
	response, err := c.Request(ctx, "tools/call", map[string]any{"name": name, "arguments": arguments})
	if err != nil {
		return nil, err
	}
	if response["error"] != nil {
		return nil, fmt.Errorf("tools/call %s: %v", name, response["error"])
	}
	result, _ := response["result"].(map[string]any)
	return result, nil
}

func (c *Client) writeMessage(message map[string]any) error {
	data, err := json.Marshal(message)
	if err != nil {
		return err
	}
	c.write.Lock()
	defer c.write.Unlock()
	_, err = c.output.Write(append(data, '\n'))
	return err
}

func (c *Client) readLoop(input io.Reader) {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 64*1024), 1<<20)
	for scanner.Scan() {
		var message map[string]any
		if err := json.Unmarshal(scanner.Bytes(), &message); err != nil {
			continue
		}
		id, present := message["id"]
		if !present {
			continue
		}
		key := fmt.Sprint(id)
		c.mu.Lock()
		response := c.pending[key]
		delete(c.pending, key)
		c.mu.Unlock()
		if response != nil {
			response <- clientResponse{message: message}
		}
	}
	c.mu.Lock()
	c.err = scanner.Err()
	for key, response := range c.pending {
		response <- clientResponse{err: io.ErrUnexpectedEOF}
		delete(c.pending, key)
	}
	c.mu.Unlock()
	close(c.done)
}

func (c *Client) removePending(key string) {
	c.mu.Lock()
	delete(c.pending, key)
	c.mu.Unlock()
}

type ProcessClient struct {
	*Client
	command *exec.Cmd
	stdin   io.WriteCloser
	done    chan error
}

func StartProcessClient(ctx context.Context, executable string, arguments []string, environment []string) (*ProcessClient, error) {
	command := exec.CommandContext(ctx, executable, arguments...)
	if environment != nil {
		command.Env = append([]string(nil), environment...)
	}
	stdin, err := command.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := command.StdoutPipe()
	if err != nil {
		return nil, err
	}
	command.Stderr = os.Stderr
	if err := command.Start(); err != nil {
		return nil, err
	}
	process := &ProcessClient{command: command, stdin: stdin, done: make(chan error, 1)}
	process.Client = NewClient(stdout, stdin)
	go func() { process.done <- command.Wait() }()
	return process, nil
}

func (p *ProcessClient) Close() error {
	if p == nil || p.command == nil {
		return nil
	}
	_ = p.stdin.Close()
	select {
	case err := <-p.done:
		if err != nil && !errors.Is(err, os.ErrProcessDone) {
			return err
		}
		return nil
	default:
		if p.command.Process != nil {
			_ = p.command.Process.Kill()
		}
		return <-p.done
	}
}
