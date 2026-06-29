package main

import (
	"bytes"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

func init() {
	os.Setenv("HOSV_BASHRC_PATH", "skip")
}

func TestSchemaDoesNotLeakSecrets(t *testing.T) {
	t.Setenv("BYTEPLUS_ARK_API_KEY", "unit_test_key_that_must_not_be_printed")
	var stdout bytes.Buffer
	code := run([]string{"schema"}, strings.NewReader(""), &stdout, &bytes.Buffer{})
	if code != 0 {
		t.Fatalf("schema exit code: %d", code)
	}
	if strings.Contains(stdout.String(), "unit_test_key_that_must_not_be_printed") {
		t.Fatalf("schema leaked API key: %s", stdout.String())
	}
	var parsed map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed["ok"] != true || parsed["api_key_configured"] != true {
		t.Fatalf("unexpected schema response: %#v", parsed)
	}
}

func TestSelfTestMissingAPIKey(t *testing.T) {
	t.Setenv("BYTEPLUS_ARK_API_KEY", "")
	t.Setenv("ARK_API_KEY", "")
	var stdout bytes.Buffer
	code := run([]string{"self-test"}, strings.NewReader(""), &stdout, &bytes.Buffer{})
	if code == 0 {
		t.Fatalf("expected self-test failure without key")
	}
	if !strings.Contains(stdout.String(), `"error_code":"missing_api_key"`) {
		t.Fatalf("unexpected self-test response: %s", stdout.String())
	}
}

func TestInvalidJSON(t *testing.T) {
	var stdout bytes.Buffer
	code := run([]string{"get"}, strings.NewReader("{"), &stdout, &bytes.Buffer{})
	if code == 0 {
		t.Fatalf("expected invalid JSON failure")
	}
	if !strings.Contains(stdout.String(), `"error_code":"invalid_request"`) {
		t.Fatalf("unexpected response: %s", stdout.String())
	}
}
