package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/YmSaki/PlaySpectra/mcp"
	"github.com/YmSaki/PlaySpectra/playspectra"
	"github.com/YmSaki/PlaySpectra/protocol"
)

const version = "0.1.0"

func main() { os.Exit(run(os.Args[1:])) }

func run(args []string) int {
	if len(args) == 0 {
		usage()
		return 2
	}
	switch args[0] {
	case "cmd":
		return commandCmd(args[1:])
	case "run":
		return commandRun(args[1:])
	case "mcp":
		return commandMCP(args[1:])
	case "record":
		return commandRecord(args[1:])
	case "replay":
		return commandReplay(args[1:])
	case "doctor":
		return commandDoctor(args[1:])
	case "session":
		return commandSession(args[1:])
	case "version", "--version":
		fmt.Println(version)
		return 0
	case "--cmd": // compatibility with tools/playspectra_server.py
		if len(args) < 2 {
			fmt.Fprintln(os.Stderr, "--cmd requires an operation")
			return 2
		}
		return commandCmd(append([]string{args[1]}, args[2:]...))
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n", args[0])
		usage()
		return 2
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "PlaySpectra Go control plane")
	fmt.Fprintln(os.Stderr, "usage: playspectra cmd <operation> | run <scenario.json> | mcp | record | replay | doctor | session")
}

func commandCmd(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "cmd requires an operation")
		return 2
	}
	operation := normalizeCommand(args[0])
	fs := flag.NewFlagSet("playspectra cmd", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "adapter host")
	port := fs.Int("port", 52702, "adapter port")
	capturePort := fs.Int("capture-port", 0, "layer capture port")
	rate := fs.Float64("rate", 60, "interpolation rate")
	argsJSON := fs.String("args", "", "JSON object of operation arguments")
	defaults := defaultCommandOptions(operation)
	x := fs.Float64("x", defaults.X, "STAGE x")
	y := fs.Float64("y", defaults.Y, "STAGE y")
	z := fs.Float64("z", defaults.Z, "STAGE z")
	duration := fs.Int("duration-ms", defaults.DurationMS, "operation duration")
	yaw := fs.Float64("yaw-deg", defaults.YawDeg, "head yaw")
	speed := fs.Float64("speed", defaults.Speed, "stick speed")
	hand := fs.String("hand", defaults.Hand, "controller hand")
	value := fs.Float64("value", defaults.Value, "numeric input value")
	button := fs.String("button", defaults.Button, "button")
	path := fs.String("path", defaults.Path, "semantic controller input path")
	ms := fs.Int("ms", defaults.MS, "button hold duration")
	if err := fs.Parse(args[1:]); err != nil {
		return 2
	}
	operationArgs := map[string]any{}
	if *argsJSON != "" {
		if err := json.Unmarshal([]byte(*argsJSON), &operationArgs); err != nil {
			fmt.Fprintln(os.Stderr, "bad --args JSON:", err)
			return 2
		}
	} else {
		operationArgs = operationArguments(operation, commandOptions{
			X: *x, Y: *y, Z: *z, DurationMS: *duration, YawDeg: *yaw, Speed: *speed,
			Hand: *hand, Value: *value, Button: *button, Path: *path, MS: *ms,
		})
	}
	client, server, closeFn, err := openServer(*host, *port, *rate, *capturePort)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	defer closeFn()
	ctx := context.Background()
	if err := server.Hello(ctx, "writer"); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	var result any
	if operation != "get_state" && operation != "status" {
		result, err = server.RunStep(ctx, merge(map[string]any{"cmd": operation}, operationArgs))
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	state, stateErr := server.GetState(ctx)
	if stateErr != nil {
		fmt.Fprintln(os.Stderr, stateErr)
		return 2
	}
	output, _ := json.Marshal(map[string]any{"cmd": operation, "result": result, "state": state})
	fmt.Println(string(output))
	if ok, isBool := result.(bool); isAssertionCommand(operation) && isBool && !ok {
		return 1
	}
	_ = client
	return 0
}

func commandRun(args []string) int {
	path, flagArgs := firstPathAndFlags(args)
	fs := flag.NewFlagSet("playspectra run", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "adapter host")
	port := fs.Int("port", 52702, "adapter port")
	rate := fs.Float64("rate", 60, "interpolation rate")
	capturePort := fs.Int("capture-port", 0, "layer capture port")
	if err := fs.Parse(flagArgs); err != nil {
		return 2
	}
	if path == "" {
		fmt.Fprintln(os.Stderr, "run requires a scenario JSON file")
		return 2
	}
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	_, server, closeFn, err := openServer(*host, *port, *rate, *capturePort)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	defer closeFn()
	summary, err := server.RunScenarioJSON(context.Background(), data)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	output, _ := json.Marshal(summary)
	fmt.Println(string(output))
	if !boolValue(summary["ok"]) {
		return 1
	}
	return 0
}

func commandMCP(args []string) int {
	fs := flag.NewFlagSet("playspectra mcp", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "adapter host")
	port := fs.Int("port", 52702, "adapter port")
	capturePort := fs.Int("capture-port", 52700, "layer capture port")
	rate := fs.Float64("rate", 60, "interpolation rate")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	var closeServer func()
	defer func() {
		if closeServer != nil {
			closeServer()
		}
	}()
	handler := mcp.NewHandler(func(ctx context.Context) (*playspectra.Server, error) {
		_, server, closeFn, err := openServer(*host, *port, *rate, *capturePort)
		if err != nil {
			return nil, err
		}
		closeServer = closeFn
		if err := server.Hello(ctx, "writer"); err != nil {
			return nil, err
		}
		return server, nil
	})
	if err := handler.Serve(context.Background(), os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}

func commandRecord(args []string) int {
	path, flagArgs := firstPathAndFlags(args)
	fs := flag.NewFlagSet("playspectra record", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "adapter host")
	port := fs.Int("port", 52702, "adapter port")
	rate := fs.Float64("rate", 60, "sample rate")
	duration := fs.Int("duration-ms", 3000, "record duration")
	if err := fs.Parse(flagArgs); err != nil {
		return 2
	}
	if path == "" {
		fmt.Fprintln(os.Stderr, "record requires an output JSON file")
		return 2
	}
	client, err := protocol.Dial(context.Background(), net.JoinHostPort(*host, strconv.Itoa(*port)))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	defer client.Close()
	recorder := playspectra.NewRecorder(client, *rate)
	if err := recorder.Hello(context.Background()); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if err := recorder.RecordFor(context.Background(), time.Duration(*duration)*time.Millisecond); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if err := playspectra.SaveRecording(path, recorder.Recording(path)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	fmt.Printf("saved %d frames to %s\n", len(recorder.Frames), path)
	return 0
}

func commandReplay(args []string) int {
	path, flagArgs := firstPathAndFlags(args)
	fs := flag.NewFlagSet("playspectra replay", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "adapter host")
	port := fs.Int("port", 52702, "adapter port")
	if err := fs.Parse(flagArgs); err != nil {
		return 2
	}
	if path == "" {
		fmt.Fprintln(os.Stderr, "replay requires a recording JSON file")
		return 2
	}
	recording, err := playspectra.LoadRecording(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	client, err := protocol.Dial(context.Background(), net.JoinHostPort(*host, strconv.Itoa(*port)))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	defer client.Close()
	replayer := playspectra.NewReplayer(client)
	if err := replayer.Hello(context.Background()); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if _, err := replayer.Play(context.Background(), recording, func(format string, values ...any) { fmt.Printf(format+"\n", values...) }); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	return 0
}

func commandDoctor(args []string) int {
	fs := flag.NewFlagSet("playspectra doctor", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	host := fs.String("host", "127.0.0.1", "host")
	port := fs.Int("port", 52702, "adapter port")
	capturePort := fs.Int("capture-port", 52700, "capture port")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	result := map[string]any{"version": version, "operate": probe(*host, *port), "capture": probe(*host, *capturePort)}
	data, _ := json.Marshal(result)
	fmt.Println(string(data))
	if !boolValue(result["operate"].(map[string]any)["reachable"]) {
		return 1
	}
	return 0
}

func commandSession(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "session requires start, status, or stop")
		return 2
	}
	fs := flag.NewFlagSet("playspectra session", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	pidFile := fs.String("pid-file", ".playspectra-session.json", "session record path")
	command := fs.String("command", "", "executable to start")
	argsJSON := fs.String("args", "[]", "JSON array of process arguments")
	if err := fs.Parse(args[1:]); err != nil {
		return 2
	}
	switch args[0] {
	case "start":
		var processArgs []string
		if err := json.Unmarshal([]byte(*argsJSON), &processArgs); err != nil {
			fmt.Fprintln(os.Stderr, "bad --args JSON:", err)
			return 2
		}
		record, err := playspectra.StartSession(*pidFile, *command, processArgs...)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		printJSON(record)
		return 0
	case "status":
		status, err := playspectra.SessionStatus(*pidFile)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		printJSON(status)
		if running, _ := status["running"].(bool); !running {
			return 1
		}
		return 0
	case "stop":
		if err := playspectra.StopSession(*pidFile); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		printJSON(map[string]any{"stopped": true})
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown session command %q\n", args[0])
		return 2
	}
}

func probe(host string, port int) map[string]any {
	conn, err := net.DialTimeout("tcp", net.JoinHostPort(host, strconv.Itoa(port)), time.Second)
	if err != nil {
		return map[string]any{"host": host, "port": port, "reachable": false, "error": err.Error()}
	}
	_ = conn.Close()
	return map[string]any{"host": host, "port": port, "reachable": true}
}

func openServer(host string, port int, rate float64, capturePort int) (*protocol.Client, *playspectra.Server, func(), error) {
	client, err := protocol.Dial(context.Background(), net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		return nil, nil, func() {}, err
	}
	options := []playspectra.ServerOption{
		playspectra.WithRate(rate),
		playspectra.WithLogger(func(format string, values ...any) { fmt.Fprintf(os.Stderr, format+"\n", values...) }),
	}
	var capture *protocol.Client
	if capturePort > 0 {
		capture, _ = protocol.Dial(context.Background(), net.JoinHostPort(host, strconv.Itoa(capturePort)))
		if capture != nil {
			options = append(options, playspectra.WithCapture(capture))
		}
	}
	server := playspectra.NewServer(client, options...)
	closeFn := func() {
		client.Close()
		if capture != nil {
			capture.Close()
		}
	}
	return client, server, closeFn, nil
}

func firstPathAndFlags(args []string) (string, []string) {
	path := ""
	flags := []string{}
	for i := 0; i < len(args); i++ {
		if strings.HasPrefix(args[i], "--") {
			flags = append(flags, args[i])
			if i+1 < len(args) && !strings.HasPrefix(args[i+1], "--") {
				flags = append(flags, args[i+1])
				i++
			}
		} else if path == "" {
			path = args[i]
		}
	}
	return path, flags
}
func normalizeCommand(command string) string {
	return strings.ReplaceAll(strings.TrimSpace(command), "-", "_")
}
func defaultDuration(operation string) int {
	switch operation {
	case "move_head", "look":
		return 500
	case "walk_forward", "strafe":
		return 1000
	case "trigger", "set_trigger":
		return 200
	case "move_controller":
		return 400
	default:
		return 0
	}
}

type commandOptions struct {
	X, Y, Z    float64
	DurationMS int
	YawDeg     float64
	Speed      float64
	Hand       string
	Value      float64
	Button     string
	Path       string
	MS         int
}

func defaultCommandOptions(operation string) commandOptions {
	hand := "left"
	switch operation {
	case "trigger", "set_trigger", "move_controller", "set_input", "press":
		hand = "right"
	}
	return commandOptions{
		Y: 1.6, DurationMS: defaultDuration(operation), Speed: 1,
		Hand: hand, Value: 1, Button: "a", MS: 120,
	}
}

func operationArguments(operation string, options commandOptions) map[string]any {
	switch operation {
	case "move_head":
		return map[string]any{"to": map[string]any{"position": []any{options.X, options.Y, options.Z}}, "duration_ms": options.DurationMS}
	case "look":
		return map[string]any{"yaw_deg": options.YawDeg, "duration_ms": options.DurationMS}
	case "walk_forward", "strafe":
		return map[string]any{"speed": options.Speed, "duration_ms": options.DurationMS, "hand": options.Hand}
	case "trigger", "set_trigger":
		return map[string]any{"value": options.Value, "duration_ms": options.DurationMS, "hand": options.Hand}
	case "move_controller":
		return map[string]any{"hand": options.Hand, "to": map[string]any{"position": []any{options.X, options.Y, options.Z}}, "duration_ms": options.DurationMS}
	case "set_input":
		return map[string]any{"hand": options.Hand, "path": options.Path, "value": options.Value, "duration_ms": options.DurationMS}
	case "press":
		return map[string]any{"hand": options.Hand, "button": options.Button, "ms": options.MS}
	default:
		return map[string]any{}
	}
}

func isAssertionCommand(operation string) bool {
	return operation == "assert" || operation == "wait_for" || operation == "assert_capture"
}
func merge(a, b map[string]any) map[string]any {
	out := map[string]any{}
	for k, v := range a {
		out[k] = v
	}
	for k, v := range b {
		out[k] = v
	}
	return out
}
func boolValue(value any) bool { v, _ := value.(bool); return v }

func printJSON(value any) { data, _ := json.Marshal(value); fmt.Println(string(data)) }
