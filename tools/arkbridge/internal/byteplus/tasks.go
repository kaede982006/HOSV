package byteplus

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"time"

	"arkbridge/internal/bridge"
)

type contentPart struct {
	Type     string          `json:"type"`
	Text     string          `json:"text,omitempty"`
	ImageURL *imageURLObject `json:"image_url,omitempty"`
	VideoURL *urlObject      `json:"video_url,omitempty"`
	AudioURL *urlObject      `json:"audio_url,omitempty"`
}

type imageURLObject struct {
	URL string `json:"url"`
}

type urlObject struct {
	URL string `json:"url"`
}

type createPayload struct {
	Model          string            `json:"model"`
	Content        []contentPart     `json:"content"`
	Ratio          string            `json:"ratio,omitempty"`
	Resolution     string            `json:"resolution,omitempty"`
	Duration       int               `json:"duration,omitempty"`
	FPS            int               `json:"fps,omitempty"`
	Seed           *int64            `json:"seed,omitempty"`
	Watermark      bool              `json:"watermark"`
	NegativePrompt string            `json:"negative_prompt,omitempty"`
	Metadata       map[string]string `json:"metadata,omitempty"`
}

func (c *Client) Create(ctx context.Context, req bridge.CreateRequest) bridge.Response {
	payload, err := c.BuildCreatePayload(req)
	if err != nil {
		return errorResponse(err)
	}
	raw, err := c.do(ctx, http.MethodPost, "/contents/generations/tasks", payload)
	if err != nil {
		return errorResponse(err)
	}
	return parseTask(raw)
}

func (c *Client) BuildCreatePayload(req bridge.CreateRequest) (createPayload, error) {
	req, err := normalizeCreateRequest(req)
	if err != nil {
		return createPayload{}, err
	}
	model := selectModel(c.cfg.Model, c.cfg.FastModel, req)
	content, err := buildContent(req)
	if err != nil {
		return createPayload{}, err
	}
	return createPayload{
		Model:          model,
		Content:        content,
		Ratio:          req.Ratio,
		Resolution:     req.Resolution,
		Duration:       req.DurationSeconds,
		FPS:            req.FPS,
		Seed:           req.Seed,
		Watermark:      req.Watermark,
		NegativePrompt: req.NegativePrompt,
		Metadata:       req.Metadata,
	}, nil
}

func normalizeCreateRequest(req bridge.CreateRequest) (bridge.CreateRequest, error) {
	req.Mode = strings.TrimSpace(req.Mode)
	if req.Mode == "" {
		if len(req.ImageURLs) > 0 {
			req.Mode = "image_to_video"
		} else {
			req.Mode = "text_to_video"
		}
	}
	if req.Prompt == "" {
		return req, schemaError("prompt is required")
	}
	if req.Ratio == "" {
		req.Ratio = "16:9"
	}
	if req.Resolution == "" {
		req.Resolution = "720p"
	}
	if req.DurationSeconds == 0 {
		req.DurationSeconds = 5
	}
	if req.DurationSeconds < 1 || req.DurationSeconds > 30 {
		return req, schemaError("duration_seconds must be between 1 and 30")
	}
	if req.Mode == "image_to_video" && len(req.ImageURLs) == 0 {
		return req, schemaError("image_to_video requires image_urls")
	}
	if req.Mode == "reference_to_video" && len(req.ImageURLs)+len(req.VideoURLs) == 0 {
		return req, schemaError("reference_to_video requires image_urls or video_urls")
	}
	return req, nil
}

func selectModel(defaultModel, fastModel string, req bridge.CreateRequest) string {
	if req.Model != "" {
		return req.Model
	}
	if req.FastMode {
		return fastModel
	}
	return defaultModel
}

func buildContent(req bridge.CreateRequest) ([]contentPart, error) {
	content := []contentPart{{Type: "text", Text: req.Prompt}}
	for _, image := range req.ImageURLs {
		if err := validateRemoteURL(image); err != nil {
			return nil, APIError{Code: "invalid_media_url", Message: err.Error()}
		}
		content = append(content, contentPart{Type: "image_url", ImageURL: &imageURLObject{URL: image}})
	}
	for _, video := range req.VideoURLs {
		if err := validateRemoteURL(video); err != nil {
			return nil, APIError{Code: "invalid_media_url", Message: err.Error()}
		}
		content = append(content, contentPart{Type: "video_url", VideoURL: &urlObject{URL: video}})
	}
	for _, audio := range req.AudioURLs {
		if err := validateRemoteURL(audio); err != nil {
			return nil, APIError{Code: "invalid_media_url", Message: err.Error()}
		}
		content = append(content, contentPart{Type: "audio_url", AudioURL: &urlObject{URL: audio}})
	}
	return content, nil
}

func (c *Client) Get(ctx context.Context, taskID string) bridge.Response {
	if taskID == "" {
		return bridge.Error("invalid_request", "task_id is required", false)
	}
	raw, err := c.do(ctx, http.MethodGet, "/contents/generations/tasks/"+taskID, nil)
	if err != nil {
		return errorResponse(err)
	}
	return parseTask(raw)
}

func (c *Client) Cancel(ctx context.Context, taskID string) bridge.Response {
	if taskID == "" {
		return bridge.Error("invalid_request", "task_id is required", false)
	}
	raw, err := c.do(ctx, http.MethodDelete, "/contents/generations/tasks/"+taskID, nil)
	if err != nil {
		return errorResponse(err)
	}
	if len(raw) == 0 || string(raw) == "null" {
		raw = json.RawMessage(`{}`)
	}
	out := parseTask(raw)
	if out.TaskID == "" {
		out.TaskID = taskID
	}
	if out.Status == "" || out.Status == "unknown" {
		out.Status = "cancelled"
	}
	return out
}

func (c *Client) Wait(ctx context.Context, req bridge.TaskRequest) bridge.Response {
	if req.TaskID == "" {
		return bridge.Error("invalid_request", "task_id is required", false)
	}
	initial := durationWithFallback(req.PollInitialMS, c.cfg.PollInitial)
	maxDelay := durationWithFallback(req.PollMaxMS, c.cfg.PollMax)
	timeout := durationWithFallback(req.PollTimeoutMS, c.cfg.PollTimeout)
	deadline, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	delay := initial
	for {
		select {
		case <-deadline.Done():
			return pollingContextError(deadline.Err())
		default:
		}
		result := c.Get(deadline, req.TaskID)
		if !result.OK {
			if result.Retryable {
				if !waitForPollDelay(deadline, delay) {
					return pollingContextError(deadline.Err())
				}
				delay = nextDelay(delay, maxDelay)
				continue
			}
			return result
		}
		if IsTerminal(result.Status) {
			return result
		}
		if !waitForPollDelay(deadline, delay) {
			return pollingContextError(deadline.Err())
		}
		delay = nextDelay(delay, maxDelay)
	}
}

func waitForPollDelay(ctx context.Context, delay time.Duration) bool {
	timer := time.NewTimer(delay)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func pollingContextError(err error) bridge.Response {
	if err == context.Canceled {
		return bridge.Error("cancelled", "polling cancelled", true)
	}
	return bridge.Error("polling_timeout", "polling timed out", true)
}

func nextDelay(current, max time.Duration) time.Duration {
	next := current * 2
	if next > max {
		return max
	}
	return next
}
