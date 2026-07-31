package protocol

import (
	"bufio"
	"context"
	"net"
	"testing"
)

func TestClientMatchesRequestIDAndSkipsEvents(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	done := make(chan error, 1)
	go func() {
		reader := bufio.NewReader(server)
		if _, err := reader.ReadString('\n'); err != nil {
			done <- err
			return
		}
		_, err := server.Write([]byte(`{"event":"haptics"}` + "\n" + `{"request_id":"r1","ok":true}` + "\n"))
		done <- err
	}()
	response, err := client.Request(context.Background(), map[string]any{"cmd": "get_state", "request_id": "r1"})
	if err != nil {
		t.Fatal(err)
	}
	if response["ok"] != true {
		t.Fatalf("response = %v", response)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestClientRequestLineDoesNotRequireRequestID(t *testing.T) {
	server, clientConn := net.Pipe()
	defer server.Close()
	client := NewClient(clientConn)
	go func() {
		reader := bufio.NewReader(server)
		_, _ = reader.ReadString('\n')
		_, _ = server.Write([]byte(`{"ok":true,"path":"capture.png"}` + "\n"))
	}()
	response, err := client.RequestLine(context.Background(), map[string]any{"cmd": "screenshot"})
	if err != nil || response["path"] != "capture.png" {
		t.Fatalf("response=%v err=%v", response, err)
	}
}
