package byteplus

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"arkbridge/internal/bridge"
	"arkbridge/internal/config"
)

func testClient(handler http.HandlerFunc) *Client {
	server := httptest.NewServer(handler)
	cfg := config.Load()
	cfg.APIKey = "test-key"
	cfg.HasAPIKey = true
	cfg.BaseURL = server.URL
	cfg.Timeout = time.Second
	cfg.PollInitial = time.Millisecond
	cfg.PollMax = time.Millisecond
	cfg.PollTimeout = 20 * time.Millisecond
	return New(cfg, server.Client())
}

func TestBuildCreatePayload(t *testing.T) {
	c := New(config.Config{Model: "dreamina-seedance-2-0-260128", FastModel: "fast"}, nil)
	signedImageURL := "https://example.com/a.png?" + "sig=unit-test-placeholder"
	payload, err := c.BuildCreatePayload(bridge.CreateRequest{
		Mode:            "image_to_video",
		Prompt:          "hello",
		ImageURLs:       []string{signedImageURL},
		Ratio:           "16:9",
		Resolution:      "720p",
		DurationSeconds: 5,
	})
	if err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(payload)
	if !json.Valid(data) || payload.Content[1].ImageURL.URL == "" {
		t.Fatalf("bad payload: %s", data)
	}
}

func TestBuildCreatePayloadVariants(t *testing.T) {
	c := New(config.Config{Model: "default-model", FastModel: "fast-model"}, nil)
	imageURL := "https://example.com/image.png?" + "sig=unit-test-placeholder"
	videoURL := "https://example.com/video.mp4?" + "sig=unit-test-placeholder"
	audioURL := "https://example.com/audio.wav?" + "sig=unit-test-placeholder"

	cases := []struct {
		name         string
		req          bridge.CreateRequest
		wantModel    string
		wantTypes    []string
		wantDuration int
	}{
		{
			name:         "text to video",
			req:          bridge.CreateRequest{Mode: "text_to_video", Prompt: "hello"},
			wantModel:    "default-model",
			wantTypes:    []string{"text"},
			wantDuration: 5,
		},
		{
			name:         "image to video",
			req:          bridge.CreateRequest{Mode: "image_to_video", Prompt: "hello", ImageURLs: []string{imageURL}},
			wantModel:    "default-model",
			wantTypes:    []string{"text", "image_url"},
			wantDuration: 5,
		},
		{
			name:         "reference image",
			req:          bridge.CreateRequest{Mode: "reference_to_video", Prompt: "hello", ImageURLs: []string{imageURL}},
			wantModel:    "default-model",
			wantTypes:    []string{"text", "image_url"},
			wantDuration: 5,
		},
		{
			name:         "reference video",
			req:          bridge.CreateRequest{Mode: "reference_to_video", Prompt: "hello", VideoURLs: []string{videoURL}},
			wantModel:    "default-model",
			wantTypes:    []string{"text", "video_url"},
			wantDuration: 5,
		},
		{
			name:         "audio url",
			req:          bridge.CreateRequest{Mode: "text_to_video", Prompt: "hello", AudioURLs: []string{audioURL}},
			wantModel:    "default-model",
			wantTypes:    []string{"text", "audio_url"},
			wantDuration: 5,
		},
		{
			name:         "fast model",
			req:          bridge.CreateRequest{Mode: "text_to_video", Prompt: "hello", FastMode: true},
			wantModel:    "fast-model",
			wantTypes:    []string{"text"},
			wantDuration: 5,
		},
		{
			name:         "explicit model",
			req:          bridge.CreateRequest{Mode: "text_to_video", Prompt: "hello", Model: "explicit-model", FastMode: true, DurationSeconds: 7},
			wantModel:    "explicit-model",
			wantTypes:    []string{"text"},
			wantDuration: 7,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			payload, err := c.BuildCreatePayload(tc.req)
			if err != nil {
				t.Fatal(err)
			}
			assertJSONRoundTrips(t, payload)
			if payload.Model != tc.wantModel {
				t.Fatalf("model: got %q want %q", payload.Model, tc.wantModel)
			}
			if payload.Duration != tc.wantDuration {
				t.Fatalf("duration: got %d want %d", payload.Duration, tc.wantDuration)
			}
			gotTypes := make([]string, 0, len(payload.Content))
			for _, part := range payload.Content {
				gotTypes = append(gotTypes, part.Type)
			}
			if !sameStrings(gotTypes, tc.wantTypes) {
				t.Fatalf("content types: got %#v want %#v", gotTypes, tc.wantTypes)
			}
		})
	}
}

func TestMissingLocalMediaPathRejected(t *testing.T) {
	c := New(config.Config{Model: "m"}, nil)
	_, err := c.BuildCreatePayload(bridge.CreateRequest{Prompt: "hello", ImageURLs: []string{"/tmp/hosv-missing-local-media.png"}})
	if err == nil {
		t.Fatalf("expected invalid media path")
	}
}

func TestLocalImageMediaPathIsInlined(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "reference.png")
	if err := os.WriteFile(path, []byte{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, 0600); err != nil {
		t.Fatal(err)
	}
	c := New(config.Config{Model: "m"}, nil)
	payload, err := c.BuildCreatePayload(bridge.CreateRequest{
		Mode:      "image_to_video",
		Prompt:    "hello",
		ImageURLs: []string{path},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(payload.Content) != 2 || payload.Content[1].ImageURL == nil {
		t.Fatalf("bad content: %#v", payload.Content)
	}
	if !strings.HasPrefix(payload.Content[1].ImageURL.URL, "data:image/png;base64,") {
		t.Fatalf("local image was not converted to data URL: %q", payload.Content[1].ImageURL.URL)
	}
}

func TestCreateSuccessAndAuthHeader(t *testing.T) {
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer test-key" {
			t.Fatalf("missing auth header")
		}
		if r.URL.Path != "/contents/generations/tasks" {
			t.Fatalf("unexpected path: %s", r.URL.Path)
		}
		_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"queued"}}`))
	})
	out := c.Create(context.Background(), bridge.CreateRequest{Prompt: "hello"})
	if !out.OK || out.TaskID != "task-1" || out.Status != "queued" {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func TestGetFailedTaskNormalization(t *testing.T) {
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"data":{"task_id":"task-1","status":"error","error":{"message":"bad"}}}`))
	})
	out := c.Get(context.Background(), "task-1")
	if out.Status != "failed" {
		t.Fatalf("unexpected status: %+v", out)
	}
}

func TestRateLimitResponse(t *testing.T) {
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusTooManyRequests)
		_, _ = w.Write([]byte(`{"message":"slow down"}`))
	})
	out := c.Get(context.Background(), "task-1")
	if out.OK || out.ErrorCode != "rate_limited" || !out.Retryable {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func TestPollingSuccess(t *testing.T) {
	calls := 0
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if calls < 2 {
			_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"running"}}`))
			return
		}
		_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"succeeded","video_url":"https://example.com/out.mp4"}}`))
	})
	out := c.Wait(context.Background(), bridge.TaskRequest{TaskID: "task-1", PollInitialMS: 1, PollMaxMS: 1, PollTimeoutMS: 100})
	if !out.OK || out.Status != "succeeded" {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func TestPollingTimeout(t *testing.T) {
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"running"}}`))
	})
	out := c.Wait(context.Background(), bridge.TaskRequest{TaskID: "task-1", PollInitialMS: 1, PollMaxMS: 1, PollTimeoutMS: 3})
	if out.ErrorCode != "polling_timeout" {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func TestPollingRetryableThenSuccess(t *testing.T) {
	calls := 0
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if calls == 1 {
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = w.Write([]byte(`{"message":"slow down"}`))
			return
		}
		_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"completed","video_url":"https://example.com/out.mp4"}}`))
	})
	out := c.Wait(context.Background(), bridge.TaskRequest{TaskID: "task-1", PollInitialMS: 1, PollMaxMS: 1, PollTimeoutMS: 100})
	if !out.OK || out.Status != "completed" || calls != 2 {
		t.Fatalf("unexpected response calls=%d out=%+v", calls, out)
	}
}

func TestPollingContextCancel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	c := testClient(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"data":{"id":"task-1","status":"running"}}`))
		cancel()
	})
	out := c.Wait(ctx, bridge.TaskRequest{TaskID: "task-1", PollInitialMS: 1000, PollMaxMS: 1000, PollTimeoutMS: 5000})
	if out.ErrorCode != "cancelled" {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func TestParseTaskShapes(t *testing.T) {
	cases := []struct {
		name         string
		raw          string
		wantID       string
		wantStatus   string
		wantVideo    string
		wantProgress int
		wantError    string
	}{
		{name: "root id", raw: `{"id":"task-1","status":"running","progress":12}`, wantID: "task-1", wantStatus: "running", wantProgress: 12},
		{name: "root task id", raw: `{"task_id":"task-2","status":"complete","video_url":"https://example.com/a.mp4"}`, wantID: "task-2", wantStatus: "completed", wantVideo: "https://example.com/a.mp4"},
		{name: "data object", raw: `{"data":{"id":"task-3","status":"success","output_url":"https://example.com/b.mp4"}}`, wantID: "task-3", wantStatus: "succeeded", wantVideo: "https://example.com/b.mp4"},
		{name: "task object", raw: `{"task":{"taskId":"task-4","state":"in_progress","progress":55}}`, wantID: "task-4", wantStatus: "processing", wantProgress: 55},
		{name: "result object", raw: `{"result":{"id":"task-5","status":"done","videos":[{"url":"https://example.com/c.mp4"}]}}`, wantID: "task-5", wantStatus: "completed", wantVideo: "https://example.com/c.mp4"},
		{name: "result array", raw: `{"id":"task-6","status":"succeeded","result":[{"video_url":"https://example.com/d.mp4"}]}`, wantID: "task-6", wantStatus: "succeeded", wantVideo: "https://example.com/d.mp4"},
		{name: "failed error", raw: `{"data":{"task_id":"task-7","status":"error","error":{"message":"bad request"}}}`, wantID: "task-7", wantStatus: "failed", wantError: "bad request"},
		{name: "unknown", raw: `{"unexpected":true}`, wantStatus: "unknown"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			out := parseTask(json.RawMessage(tc.raw))
			if !out.OK {
				t.Fatalf("unexpected error: %+v", out)
			}
			if out.TaskID != tc.wantID || out.Status != tc.wantStatus || out.VideoURL != tc.wantVideo || out.ErrorMessage != tc.wantError {
				t.Fatalf("unexpected parsed response: %+v", out)
			}
			if tc.wantProgress != 0 {
				if out.Progress == nil || *out.Progress != tc.wantProgress {
					t.Fatalf("unexpected progress: %+v", out.Progress)
				}
			}
		})
	}
}

func TestParseTaskMalformedJSON(t *testing.T) {
	out := parseTask(json.RawMessage(`{`))
	if out.OK || out.ErrorCode != "parse_error" {
		t.Fatalf("unexpected response: %+v", out)
	}
}

func assertJSONRoundTrips(t *testing.T, payload createPayload) {
	t.Helper()
	data, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	var decoded map[string]any
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded["model"] == "" || decoded["content"] == nil {
		t.Fatalf("missing required JSON fields: %s", data)
	}
}

func sameStrings(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}
