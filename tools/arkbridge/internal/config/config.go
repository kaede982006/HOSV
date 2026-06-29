package config

import (
	"bufio"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
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

func loadEnv() {
	bashrcPath := os.Getenv("HOSV_BASHRC_PATH")
	if bashrcPath == "skip" {
		return
	}
	if bashrcPath == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return
		}
		bashrcPath = filepath.Join(home, ".bashrc")
	}
	file, err := os.Open(bashrcPath)
	if err != nil {
		return
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "export") {
			rem := line[6:]
			if len(rem) > 0 && (rem[0] == ' ' || rem[0] == '\t') {
				line = strings.TrimSpace(rem)
			}
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}
		key := strings.TrimSpace(parts[0])
		val := strings.TrimSpace(parts[1])
		if len(val) >= 2 && ((val[0] == '"' && val[len(val)-1] == '"') || (val[0] == '\'' && val[len(val)-1] == '\'')) {
			val = val[1 : len(val)-1]
		}
		if key != "" && os.Getenv(key) == "" {
			os.Setenv(key, val)
		}
	}
}

func Load() Config {
	loadEnv()
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
