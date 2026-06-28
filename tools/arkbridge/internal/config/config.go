package config

import (
	"errors"
	"os"
	"strconv"
	"time"
)

const (
	DefaultBaseURL     = "https://ark.ap-southeast.bytepluses.com/api/v3"
	DefaultModel       = "dreamina-seedance-2-0-260128"
	DefaultFastModel   = "dreamina-seedance-2-0-fast-260128"
	DefaultRegion      = "ap-southeast"
	DefaultTimeoutMS   = 120000
	DefaultPollInitMS  = 3000
	DefaultPollMaxMS   = 15000
	DefaultPollTotalMS = 900000
)

type Config struct {
	APIKey        string
	BaseURL       string
	Model         string
	FastModel     string
	Region        string
	Timeout       time.Duration
	PollInitial   time.Duration
	PollMax       time.Duration
	PollTimeout   time.Duration
	Debug         bool
	HasAPIKey     bool
	APIKeyVarName string
}

func Load() Config {
	keyVar := "BYTEPLUS_ARK_API_KEY"
	key := os.Getenv(keyVar)
	if key == "" {
		keyVar = "ARK_API_KEY"
		key = os.Getenv(keyVar)
	}
	return Config{
		APIKey:        key,
		BaseURL:       getenv("BYTEPLUS_ARK_BASE_URL", DefaultBaseURL),
		Model:         getenv("BYTEPLUS_ARK_MODEL", DefaultModel),
		FastModel:     getenv("BYTEPLUS_ARK_FAST_MODEL", DefaultFastModel),
		Region:        getenv("BYTEPLUS_ARK_REGION", DefaultRegion),
		Timeout:       envDuration("BYTEPLUS_ARK_TIMEOUT_MS", DefaultTimeoutMS),
		PollInitial:   envDuration("BYTEPLUS_ARK_POLL_INITIAL_MS", DefaultPollInitMS),
		PollMax:       envDuration("BYTEPLUS_ARK_POLL_MAX_MS", DefaultPollMaxMS),
		PollTimeout:   envDuration("BYTEPLUS_ARK_POLL_TIMEOUT_MS", DefaultPollTotalMS),
		Debug:         os.Getenv("BYTEPLUS_ARK_DEBUG") == "1" || os.Getenv("BYTEPLUS_ARK_DEBUG") == "true",
		HasAPIKey:     key != "",
		APIKeyVarName: keyVar,
	}
}

func (c Config) RequireAPIKey() error {
	if c.APIKey == "" {
		return errors.New("BYTEPLUS_ARK_API_KEY or ARK_API_KEY is required")
	}
	return nil
}

func getenv(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func envDuration(name string, fallbackMS int) time.Duration {
	value := os.Getenv(name)
	if value == "" {
		return time.Duration(fallbackMS) * time.Millisecond
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed <= 0 {
		return time.Duration(fallbackMS) * time.Millisecond
	}
	return time.Duration(parsed) * time.Millisecond
}
