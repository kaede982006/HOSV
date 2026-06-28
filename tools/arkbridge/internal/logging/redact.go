package logging

import "strings"

func RedactSecret(value string) string {
	if value == "" {
		return ""
	}
	lower := strings.ToLower(value)
	prefix := "bearer"
	switch {
	case strings.HasPrefix(lower, "sk_live_"):
		prefix = "sk_live"
	case strings.HasPrefix(lower, "sk_test_"):
		prefix = "sk_test"
	}
	last := value
	if len(value) > 4 {
		last = value[len(value)-4:]
	}
	return prefix + "_****" + last
}

func RedactURL(value string) string {
	if value == "" {
		return ""
	}
	if i := strings.IndexAny(value, "?#"); i >= 0 {
		value = value[:i] + "?redacted"
	}
	if len(value) > 96 {
		return value[:48] + "...redacted..." + value[len(value)-16:]
	}
	return value
}
