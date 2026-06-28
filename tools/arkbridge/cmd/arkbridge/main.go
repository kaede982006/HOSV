package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"

	"arkbridge/internal/bridge"
	"arkbridge/internal/byteplus"
	"arkbridge/internal/config"
)

func main() {
	os.Exit(run(os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}

func run(args []string, in io.Reader, out io.Writer, errOut io.Writer) int {
	if len(args) < 1 {
		return write(out, errOut, bridge.Error("invalid_request", "command is required", false), 2)
	}
	cfg := config.Load()
	client := byteplus.New(cfg, nil)
	ctx, cancel := context.WithTimeout(context.Background(), cfg.Timeout)
	defer cancel()

	switch args[0] {
	case "create":
		var req bridge.CreateRequest
		if code, ok := read(in, out, errOut, &req); !ok {
			return code
		}
		return writeResponse(out, errOut, client.Create(ctx, req))
	case "get":
		var req bridge.TaskRequest
		if code, ok := read(in, out, errOut, &req); !ok {
			return code
		}
		return writeResponse(out, errOut, client.Get(ctx, req.TaskID))
	case "wait":
		var req bridge.TaskRequest
		if code, ok := read(in, out, errOut, &req); !ok {
			return code
		}
		return writeResponse(out, errOut, client.Wait(context.Background(), req))
	case "cancel":
		var req bridge.TaskRequest
		if code, ok := read(in, out, errOut, &req); !ok {
			return code
		}
		return writeResponse(out, errOut, client.Cancel(ctx, req.TaskID))
	case "schema":
		return write(out, errOut, map[string]any{
			"ok":       true,
			"provider": byteplus.ProviderName,
			"commands": []string{"create", "get", "wait", "cancel", "schema", "self-test"},
			"env": []string{
				"BYTEPLUS_ARK_API_KEY", "ARK_API_KEY", "BYTEPLUS_ARK_BASE_URL",
				"BYTEPLUS_ARK_MODEL", "BYTEPLUS_ARK_FAST_MODEL", "BYTEPLUS_ARK_REGION",
				"BYTEPLUS_ARK_TIMEOUT_MS", "BYTEPLUS_ARK_POLL_INITIAL_MS",
				"BYTEPLUS_ARK_POLL_MAX_MS", "BYTEPLUS_ARK_POLL_TIMEOUT_MS", "BYTEPLUS_ARK_DEBUG",
			},
			"endpoints": map[string]string{
				"create": "/contents/generations/tasks",
				"get":    "/contents/generations/tasks/{task_id}",
				"cancel": "/contents/generations/tasks/{task_id}",
			},
			"default_base_url":   cfg.BaseURL,
			"default_model":      cfg.Model,
			"default_fast":       cfg.FastModel,
			"api_key_configured": cfg.HasAPIKey,
			"api_key_source":     cfg.APIKeyVarName,
		}, 0)
	case "self-test":
		if err := cfg.RequireAPIKey(); err != nil {
			return write(out, errOut, bridge.Error("missing_api_key", err.Error(), false), 1)
		}
		return write(out, errOut, map[string]any{
			"ok":       true,
			"provider": byteplus.ProviderName,
			"message":  "configuration is valid; self-test does not call paid generation endpoints",
		}, 0)
	default:
		return write(out, errOut, bridge.Error("invalid_request", "unknown command: "+args[0], false), 2)
	}
}

func read(in io.Reader, out io.Writer, errOut io.Writer, v any) (int, bool) {
	dec := json.NewDecoder(in)
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return write(out, errOut, bridge.Error("invalid_request", "invalid stdin JSON: "+err.Error(), false), 2), false
	}
	return 0, true
}

func write(out io.Writer, errOut io.Writer, v any, code int) int {
	enc := json.NewEncoder(out)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		fmt.Fprintf(errOut, "encode response: %v\n", err)
		return 2
	}
	return code
}

func writeResponse(out io.Writer, errOut io.Writer, resp bridge.Response) int {
	code := 0
	if !resp.OK {
		code = 1
	}
	return write(out, errOut, resp, code)
}
