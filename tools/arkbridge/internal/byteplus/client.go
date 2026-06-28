package byteplus

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"arkbridge/internal/bridge"
	"arkbridge/internal/config"
)

const ProviderName = "byteplus_modelark"

type Client struct {
	cfg        config.Config
	httpClient *http.Client
}

type APIError struct {
	Code       string
	Message    string
	Retryable  bool
	StatusCode int
}

func (e APIError) Error() string {
	if e.Message != "" {
		return e.Message
	}
	return e.Code
}

func New(cfg config.Config, httpClient *http.Client) *Client {
	if httpClient == nil {
		httpClient = &http.Client{Timeout: cfg.Timeout}
	}
	return &Client{cfg: cfg, httpClient: httpClient}
}

func (c *Client) do(ctx context.Context, method, path string, body any) (json.RawMessage, error) {
	if err := c.cfg.RequireAPIKey(); err != nil {
		return nil, APIError{Code: "missing_api_key", Message: err.Error()}
	}

	var reader io.Reader
	if body != nil {
		payload, err := json.Marshal(body)
		if err != nil {
			return nil, APIError{Code: "invalid_request", Message: err.Error()}
		}
		reader = bytes.NewReader(payload)
	}

	req, err := http.NewRequestWithContext(ctx, method, strings.TrimRight(c.cfg.BaseURL, "/")+path, reader)
	if err != nil {
		return nil, APIError{Code: "invalid_request", Message: err.Error()}
	}
	req.Header.Set("Authorization", "Bearer "+c.cfg.APIKey)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, APIError{Code: "network_error", Message: err.Error(), Retryable: true}
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, APIError{Code: "network_error", Message: err.Error(), Retryable: true, StatusCode: resp.StatusCode}
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, mapHTTPError(resp.StatusCode, data)
	}
	if !json.Valid(data) {
		return nil, APIError{Code: "parse_error", Message: "provider returned invalid JSON", StatusCode: resp.StatusCode}
	}
	return json.RawMessage(data), nil
}

func mapHTTPError(status int, body []byte) APIError {
	message := string(body)
	var parsed struct {
		Error struct {
			Code    string `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
		Code    string `json:"code"`
		Message string `json:"message"`
	}
	_ = json.Unmarshal(body, &parsed)
	if parsed.Error.Message != "" {
		message = parsed.Error.Message
	} else if parsed.Message != "" {
		message = parsed.Message
	}

	code := "unknown_error"
	retryable := false
	switch status {
	case http.StatusUnauthorized:
		code = "unauthorized"
	case http.StatusForbidden:
		code = "forbidden"
	case http.StatusBadRequest:
		code = classifyBadRequest(parsed.Error.Code + " " + parsed.Code + " " + message)
	case http.StatusTooManyRequests:
		code = "rate_limited"
		retryable = true
	case http.StatusServiceUnavailable, http.StatusBadGateway, http.StatusGatewayTimeout:
		code = "provider_unavailable"
		retryable = true
	default:
		if status >= 500 {
			code = "provider_unavailable"
			retryable = true
		}
	}
	return APIError{Code: code, Message: message, Retryable: retryable, StatusCode: status}
}

func classifyBadRequest(text string) string {
	lower := strings.ToLower(text)
	switch {
	case strings.Contains(lower, "model"):
		return "invalid_model"
	case strings.Contains(lower, "media"), strings.Contains(lower, "url"), strings.Contains(lower, "image"):
		return "invalid_media_url"
	case strings.Contains(lower, "quota"):
		return "quota_exceeded"
	case strings.Contains(lower, "balance"), strings.Contains(lower, "billing"):
		return "insufficient_balance"
	default:
		return "invalid_request"
	}
}

func validateRemoteURL(value string) error {
	parsed, err := url.Parse(value)
	if err != nil || parsed.Host == "" || (parsed.Scheme != "http" && parsed.Scheme != "https") {
		return errors.New("Official BytePlus video generation requires public or signed media URLs. Local file upload is not configured.")
	}
	return nil
}

func errorResponse(err error) bridge.Response {
	var apiErr APIError
	if errors.As(err, &apiErr) {
		return bridge.Error(apiErr.Code, apiErr.Message, apiErr.Retryable)
	}
	return bridge.Error("unknown_error", err.Error(), false)
}

func durationWithFallback(ms int, fallback time.Duration) time.Duration {
	if ms <= 0 {
		return fallback
	}
	return time.Duration(ms) * time.Millisecond
}

func parseTask(raw json.RawMessage) bridge.Response {
	var root map[string]any
	if err := json.Unmarshal(raw, &root); err != nil {
		return bridge.Error("parse_error", err.Error(), false)
	}
	source := firstObject(root, "data", "task", "result")
	id := firstString(source, "id", "task_id", "taskId")
	if id == "" {
		id = firstString(root, "id", "task_id", "taskId")
	}
	status := NormalizeStatus(firstString(source, "status", "state"))
	if status == "unknown" {
		status = NormalizeStatus(firstString(root, "status", "state"))
	}
	videoURL := firstString(source, "video_url", "output_url", "url")
	if videoURL == "" {
		videoURL = firstURLFromArrays(source, "video_urls", "videos", "results")
	}
	progress := firstIntPtr(source, "progress")
	return bridge.Response{OK: true, TaskID: id, Status: status, Provider: ProviderName, Progress: progress, VideoURL: videoURL, Raw: raw}
}

func firstObject(root map[string]any, keys ...string) map[string]any {
	for _, key := range keys {
		if obj, ok := root[key].(map[string]any); ok {
			return obj
		}
	}
	return root
}

func firstString(root map[string]any, keys ...string) string {
	for _, key := range keys {
		if value, ok := root[key].(string); ok {
			return value
		}
	}
	return ""
}

func firstIntPtr(root map[string]any, keys ...string) *int {
	for _, key := range keys {
		switch value := root[key].(type) {
		case float64:
			out := int(value)
			return &out
		case int:
			out := value
			return &out
		}
	}
	return nil
}

func firstURLFromArrays(root map[string]any, keys ...string) string {
	for _, key := range keys {
		items, ok := root[key].([]any)
		if !ok {
			continue
		}
		for _, item := range items {
			if value, ok := item.(string); ok {
				return value
			}
			if obj, ok := item.(map[string]any); ok {
				if value := firstString(obj, "url", "video_url", "output_url"); value != "" {
					return value
				}
			}
		}
	}
	return ""
}

func schemaError(format string, args ...any) error {
	return APIError{Code: "invalid_request", Message: fmt.Sprintf(format, args...)}
}
