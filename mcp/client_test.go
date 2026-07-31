package mcp

import (
	"context"
	"io"
	"testing"
)

func TestClientExercisesServerOverNDJSON(t *testing.T) {
	clientInput, serverOutput := io.Pipe()
	serverInput, clientOutput := io.Pipe()
	serverDone := make(chan error, 1)
	go func() {
		serverDone <- NewHandler(nil).Serve(context.Background(), serverInput, serverOutput)
	}()
	client := NewClient(clientInput, clientOutput)
	initialize, err := client.Initialize(context.Background())
	if err != nil || initialize["protocolVersion"] != "2024-11-05" {
		t.Fatalf("initialize=%v err=%v", initialize, err)
	}
	tools, err := client.ListTools(context.Background())
	if err != nil || len(tools) != 13 || tools[0]["name"] != "move_head" {
		t.Fatalf("tools=%v err=%v", tools, err)
	}
	result, err := client.CallTool(context.Background(), "does_not_exist", map[string]any{})
	if err != nil || result["isError"] != true {
		t.Fatalf("result=%v err=%v", result, err)
	}
	_ = clientOutput.Close()
	_ = serverInput.Close()
	_ = serverOutput.Close()
	_ = clientInput.Close()
	if err := <-serverDone; err != nil && err != io.ErrClosedPipe {
		t.Fatal(err)
	}
}
