package config

import "testing"

func TestLoadFallbacks(t *testing.T) {
	t.Setenv("BYTEPLUS_ARK_API_KEY", "")
	t.Setenv("ARK_API_KEY", "ark-test")
	t.Setenv("BYTEPLUS_ARK_TIMEOUT_MS", "42")
	cfg := Load()
	if cfg.APIKey != "ark-test" || !cfg.HasAPIKey {
		t.Fatalf("fallback API key not loaded")
	}
	if cfg.Timeout.Milliseconds() != 42 {
		t.Fatalf("timeout not loaded: %s", cfg.Timeout)
	}
}

func TestMissingAPIKey(t *testing.T) {
	t.Setenv("BYTEPLUS_ARK_API_KEY", "")
	t.Setenv("ARK_API_KEY", "")
	cfg := Load()
	if cfg.RequireAPIKey() == nil {
		t.Fatalf("expected missing api key error")
	}
}
