package byteplus

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
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

func TestInvalidMediaURL(t *testing.T) {
	c := New(config.Config{Model: "m"}, nil)
	_, err := c.BuildCreatePayload(bridge.CreateRequest{Prompt: "hello", ImageURLs: []string{"/tmp/a.png"}})
	if err == nil {
		t.Fatalf("expected invalid media url")
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
