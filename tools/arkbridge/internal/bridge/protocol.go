package bridge

import "encoding/json"

type CreateRequest struct {
	Mode            string            `json:"mode"`
	Prompt          string            `json:"prompt"`
	NegativePrompt  string            `json:"negative_prompt,omitempty"`
	ImageURLs       []string          `json:"image_urls,omitempty"`
	VideoURLs       []string          `json:"video_urls,omitempty"`
	AudioURLs       []string          `json:"audio_urls,omitempty"`
	Ratio           string            `json:"ratio,omitempty"`
	Resolution      string            `json:"resolution,omitempty"`
	DurationSeconds int               `json:"duration_seconds,omitempty"`
	FPS             int               `json:"fps,omitempty"`
	Seed            *int64            `json:"seed,omitempty"`
	Model           string            `json:"model,omitempty"`
	FastMode        bool              `json:"fast_mode,omitempty"`
	Watermark       bool              `json:"watermark"`
	OutputDir       string            `json:"output_dir,omitempty"`
	Metadata        map[string]string `json:"metadata,omitempty"`
}

type TaskRequest struct {
	TaskID        string `json:"task_id"`
	PollInitialMS int    `json:"poll_initial_ms,omitempty"`
	PollMaxMS     int    `json:"poll_max_ms,omitempty"`
	PollTimeoutMS int    `json:"poll_timeout_ms,omitempty"`
}

type Response struct {
	OK           bool            `json:"ok"`
	TaskID       string          `json:"task_id,omitempty"`
	Status       string          `json:"status,omitempty"`
	Provider     string          `json:"provider,omitempty"`
	Progress     *int            `json:"progress,omitempty"`
	VideoURL     string          `json:"video_url,omitempty"`
	ErrorCode    string          `json:"error_code,omitempty"`
	ErrorMessage string          `json:"error_message,omitempty"`
	Retryable    bool            `json:"retryable"`
	Raw          json.RawMessage `json:"raw,omitempty"`
}

func Error(code, message string, retryable bool) Response {
	return Response{OK: false, ErrorCode: code, ErrorMessage: message, Retryable: retryable}
}
