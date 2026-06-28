package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"

	"arkbridge/internal/bridge"
	"arkbridge/internal/byteplus"
	"arkbridge/internal/config"
)

func main() {
	if len(os.Args) < 2 {
		write(bridge.Error("invalid_request", "command is required", false), 2)
	}
	cfg := config.Load()
	client := byteplus.New(cfg, nil)
	ctx, cancel := context.WithTimeout(context.Background(), cfg.Timeout)
	defer cancel()

	switch os.Args[1] {
	case "create":
		var req bridge.CreateRequest
		if !read(&req) {
			return
		}
		writeResponse(client.Create(ctx, req))
	case "get":
		var req bridge.TaskRequest
		if !read(&req) {
			return
		}
		writeResponse(client.Get(ctx, req.TaskID))
	case "wait":
		var req bridge.TaskRequest
		if !read(&req) {
			return
		}
		writeResponse(client.Wait(context.Background(), req))
	case "cancel":
		var req bridge.TaskRequest
		if !read(&req) {
			return
		}
		writeResponse(client.Cancel(ctx, req.TaskID))
	case "schema":
		write(map[string]any{
			"ok":       true,
			"provider": byteplus.ProviderName,
			"commands": []string{"create", "get", "wait", "cancel", "schema", "self-test"},
			"env": []string{
				"BYTEPLUS_ARK_API_KEY", "ARK_API_KEY", "BYTEPLUS_ARK_BASE_URL",
				"BYTEPLUS_ARK_MODEL", "BYTEPLUS_ARK_FAST_MODEL", "BYTEPLUS_ARK_REGION",
				"BYTEPLUS_ARK_TIMEOUT_MS", "BYTEPLUS_ARK_POLL_INITIAL_MS",
				"BYTEPLUS_ARK_POLL_MAX_MS", "BYTEPLUS_ARK_POLL_TIMEOUT_MS", "BYTEPLUS_ARK_DEBUG",
			},
			"default_base_url": cfg.BaseURL,
			"default_model":    cfg.Model,
			"default_fast":     cfg.FastModel,
		}, 0)
	case "self-test":
		if err := cfg.RequireAPIKey(); err != nil {
			write(bridge.Error("missing_api_key", err.Error(), false), 1)
		}
		write(map[string]any{"ok": true, "provider": byteplus.ProviderName, "message": "configuration is valid"}, 0)
	default:
		write(bridge.Error("invalid_request", "unknown command: "+os.Args[1], false), 2)
	}
}

func read(v any) bool {
	dec := json.NewDecoder(os.Stdin)
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		write(bridge.Error("invalid_request", "invalid stdin JSON: "+err.Error(), false), 2)
		return false
	}
	return true
}

func write(v any, code int) {
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		fmt.Fprintf(os.Stderr, "encode response: %v\n", err)
		os.Exit(2)
	}
	if code != 0 {
		os.Exit(code)
	}
}

func writeResponse(resp bridge.Response) {
	code := 0
	if !resp.OK {
		code = 1
	}
	write(resp, code)
}
