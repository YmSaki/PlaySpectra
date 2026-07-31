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
	"github.com/YmSaki/PlaySpectra/pngstats"
	"github.com/YmSaki/PlaySpectra/protocol"
	"github.com/YmSaki/PlaySpectra/setuphelper"
	"github.com/YmSaki/PlaySpectra/verify"
	"github.com/YmSaki/PlaySpectra/vrapp"
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
	case "image-stats":
		return commandImageStats(args[1:])
	case "verify":
		return commandVerify(args[1:])
	case "session":
		return commandSession(args[1:])
	case "internal":
		return commandInternal(args[1:])
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
	fmt.Fprintln(os.Stderr, "usage: playspectra cmd <operation> | run <scenario.json> | mcp | record | replay | doctor | image-stats | verify | session")
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
	var scenario map[string]any
	if err := json.Unmarshal(data, &scenario); err != nil {
		fmt.Fprintln(os.Stderr, "decode scenario:", err)
		return 2
	}
	if scenario == nil {
		fmt.Fprintln(os.Stderr, "decode scenario: top level must be an object")
		return 2
	}
	_, server, closeFn, err := openServer(*host, *port, *rate, *capturePort)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	defer closeFn()
	summary, err := server.RunScenario(context.Background(), scenario)
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
	port := fs.Int("port", envInt("PLAYSPECTRA_MONADO_PORT", 52702), "adapter port")
	capturePort := fs.Int("capture-port", envInt("PLAYSPECTRA_PORT", 52700), "layer capture port")
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

func commandImageStats(args []string) int {
	fs := flag.NewFlagSet("playspectra image-stats", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	maxRows := fs.Int("max-rows", 0, "maximum rows to decode (0 = all)")
	columns := fs.Int("columns", 200, "approximately how many columns to sample")
	sampleRows := fs.Int("sample-rows", 300, "approximately how many rows to sample")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	paths := fs.Args()
	if len(paths) == 0 {
		fmt.Fprintln(os.Stderr, "usage: playspectra image-stats <file.png> [more.png ...]")
		return 2
	}
	exitCode := 0
	for _, path := range paths {
		if _, err := os.Stat(path); err != nil {
			if os.IsNotExist(err) {
				fmt.Printf("%s: missing\n", path)
				continue
			}
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, err)
			exitCode = 1
			continue
		}
		stats, err := pngstats.Stats(path, pngstats.Options{MaxRows: *maxRows, Columns: *columns, SampleRows: *sampleRows})
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, err)
			exitCode = 1
			continue
		}
		data, _ := json.Marshal(stats)
		label := "False"
		if pngstats.IsNonDegenerate(stats, 0, 0) {
			label = "True"
		}
		fmt.Printf("%s\n  %s\n  non-degenerate: %s\n", path, data, label)
	}
	return exitCode
}

func commandVerify(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "verify requires a suite name")
		return 2
	}
	switch args[0] {
	case "vrapp":
		fs := flag.NewFlagSet("playspectra verify vrapp", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		executable := fs.String("exe", vrapp.DefaultExecutable(), "VRApp console executable")
		host := fs.String("host", "127.0.0.1", "operate host")
		port := fs.Int("port", 0, "operate port")
		capturePort := fs.Int("capture-port", 0, "capture port")
		logPath := fs.String("log", "", "VRApp combined output log")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		return vrapp.RunSuite(context.Background(), vrapp.SuiteConfig{
			Executable: *executable, OperateHost: *host, OperatePort: *port,
			CapturePort: *capturePort, LogPath: *logPath, Output: os.Stdout,
		})
	case "server", "record", "frame", "reset", "multiobs":
		fs := flag.NewFlagSet("playspectra verify "+args[0], flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		host := fs.String("host", "127.0.0.1", "operate host")
		port := fs.Int("port", 52702, "operate port")
		rate := fs.Float64("rate", 60, "operation/sample rate")
		hapticsTimeout := fs.Duration("haptics-timeout", 12*time.Second, "multi-observer haptics wait")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		address := net.JoinHostPort(*host, strconv.Itoa(*port))
		var report verify.Report
		var err error
		switch args[0] {
		case "server":
			report, err = verify.Server(context.Background(), address, *rate)
		case "record":
			report, err = verify.Recording(context.Background(), address, *rate)
		case "frame":
			report, err = verify.FrameSynchronized(context.Background(), address)
		case "reset":
			report, err = verify.Reset(context.Background(), address)
		case "multiobs":
			report, err = verify.MultiObserver(context.Background(), address, *hapticsTimeout)
		}
		return printVerification(report, err)
	case "coupling":
		fs := flag.NewFlagSet("playspectra verify coupling", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		host := fs.String("host", "127.0.0.1", "host")
		port := fs.Int("port", 52702, "operate port")
		capturePort := fs.Int("capture-port", 52700, "capture port")
		targetZ := fs.Float64("target-z", -2.5, "absolute target head z")
		tolerance := fs.Float64("tolerance", 0.3, "view pose tolerance")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		report, err := verify.Coupling(
			context.Background(), net.JoinHostPort(*host, strconv.Itoa(*port)),
			net.JoinHostPort(*host, strconv.Itoa(*capturePort)), *targetZ, *tolerance,
		)
		return printVerification(report, err)
	case "mcp":
		fs := flag.NewFlagSet("playspectra verify mcp", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		host := fs.String("host", "127.0.0.1", "operate host")
		port := fs.Int("port", 52702, "operate port")
		capturePort := fs.Int("capture-port", 52700, "capture port")
		executable, err := os.Executable()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 2
		}
		program := fs.String("executable", executable, "playspectra executable to verify")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		report, err := verify.MCP(context.Background(), *program, *host, *port, *capturePort)
		return printVerification(report, err)
	default:
		fmt.Fprintf(os.Stderr, "unknown verify suite %q\n", args[0])
		return 2
	}
}

func printVerification(report verify.Report, err error) int {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	report.WriteText(os.Stdout)
	return report.ExitCode()
}

func commandInternal(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "internal requires an operation")
		return 2
	}
	fail := func(err error) int {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	switch args[0] {
	case "vcxproj-x64":
		if len(args) != 2 {
			fmt.Fprintln(os.Stderr, "usage: playspectra internal vcxproj-x64 <path>")
			return 2
		}
		path, err := setuphelper.TransformVCXProj(args[1])
		if err != nil {
			return fail(err)
		}
		fmt.Println(path)
		return 0
	case "deploy-hellovr":
		fs := flag.NewFlagSet("playspectra internal deploy-hellovr", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		samples := fs.String("samples", "", "OpenVR samples directory")
		destination := fs.String("destination", "", "deployment directory")
		root := fs.String("root", "", "PlaySpectra source root")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		if *samples == "" || *destination == "" || *root == "" {
			fmt.Fprintln(os.Stderr, "--samples, --destination, and --root are required")
			return 2
		}
		names, err := setuphelper.DeployHelloVR(*samples, *destination, *root)
		if err != nil {
			return fail(err)
		}
		fmt.Printf("deployed: %v\n", names)
		return 0
	case "extract-monado":
		fs := flag.NewFlagSet("playspectra internal extract-monado", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		archive := fs.String("zip", "", "Monado CI archive")
		destination := fs.String("destination", "", "extraction directory")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		if *archive == "" || *destination == "" {
			fmt.Fprintln(os.Stderr, "--zip and --destination are required")
			return 2
		}
		count, err := setuphelper.ExtractMonado(*archive, *destination)
		if err != nil {
			return fail(err)
		}
		fmt.Printf("extracted %d files -> %s\n", count, *destination)
		return 0
	case "patch-helloxr":
		fs := flag.NewFlagSet("playspectra internal patch-helloxr", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		source := fs.String("source", "", "OpenXR SDK source directory")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		if *source == "" {
			fmt.Fprintln(os.Stderr, "--source is required")
			return 2
		}
		markers, err := setuphelper.PatchHelloXR(*source)
		if err != nil {
			return fail(err)
		}
		for _, marker := range markers {
			fmt.Println("patched:", marker)
		}
		return 0
	case "check-pe-x64":
		if len(args) != 2 {
			fmt.Fprintln(os.Stderr, "usage: playspectra internal check-pe-x64 <path>")
			return 2
		}
		size, err := setuphelper.CheckPEX64(args[1])
		if err != nil {
			fmt.Printf("PE check: FAILED (%d bytes; %v - download broken?)\n", size, err)
			return 1
		}
		fmt.Printf("PE check: x64 OK (%d bytes)\n", size)
		return 0
	case "wait-tcp":
		fs := flag.NewFlagSet("playspectra internal wait-tcp", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		address := fs.String("address", "127.0.0.1:52702", "TCP address")
		timeout := fs.Duration("timeout", 18*time.Second, "wait timeout")
		interval := fs.Duration("interval", 300*time.Millisecond, "retry interval")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		ctx, cancel := context.WithTimeout(context.Background(), *timeout)
		defer cancel()
		if err := setuphelper.WaitTCP(ctx, *address, *interval); err != nil {
			return fail(err)
		}
		fmt.Println("CTRL_UP")
		return 0
	case "compare-captures":
		fs := flag.NewFlagSet("playspectra internal compare-captures", flag.ContinueOnError)
		fs.SetOutput(os.Stderr)
		recording := fs.String("recording", "", "recording directory")
		baseline := fs.String("baseline", "", "baseline PNG")
		post := fs.String("post", "", "post-injection PNG")
		if err := fs.Parse(args[1:]); err != nil {
			return 2
		}
		if *recording == "" || *baseline == "" || *post == "" {
			fmt.Fprintln(os.Stderr, "--recording, --baseline, and --post are required")
			return 2
		}
		result, err := setuphelper.CompareCaptures(*recording, *baseline, *post)
		if err != nil {
			return fail(err)
		}
		fmt.Printf("frames: %d distinct contents: %d\n", result.Frames, result.Distinct)
		fmt.Printf("baseline hash: %s post hash: %s\n", result.BaselineHash, result.PostHash)
		fmt.Printf("%s capture tracks state (>=2 distinct contents)\n", passLabel(result.Distinct >= 2))
		fmt.Printf("%s post-injection frame differs from baseline\n", passLabel(result.BaselineHash != "" && result.PostHash != "" && result.BaselineHash != result.PostHash))
		if !result.OK {
			return 1
		}
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown internal operation %q\n", args[0])
		return 2
	}
}

func passLabel(ok bool) string {
	if ok {
		return "PASS"
	}
	return "FAIL"
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

func envInt(name string, fallback int) int {
	value, err := strconv.Atoi(os.Getenv(name))
	if err != nil {
		return fallback
	}
	return value
}

func printJSON(value any) { data, _ := json.Marshal(value); fmt.Println(string(data)) }
