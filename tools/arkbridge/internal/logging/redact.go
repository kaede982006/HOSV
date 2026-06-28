package logging

import (
	"regexp"
	"strings"
)

var (
	authBearerPattern = regexp.MustCompile(`(?i)\bAuthorization:\s*Bearer\s+([A-Za-z0-9._~+/\-=]{8,})`)
	envKeyPattern     = regexp.MustCompile(`(?i)\b(BYTEPLUS_ARK_API_KEY|ARK_API_KEY)=([^\s"'` + "`" + `]+)`)
	skPattern         = regexp.MustCompile(`\bsk_(live|test)_[A-Za-z0-9_]{8,}`)
	longTokenPattern  = regexp.MustCompile(`\b[A-Za-z0-9._~+/\-=]{32,}\b`)
)

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

func RedactText(value string) string {
	if value == "" {
		return ""
	}
	value = authBearerPattern.ReplaceAllString(value, "Authorization: Bearer ****")
	value = envKeyPattern.ReplaceAllString(value, "$1=****")
	value = skPattern.ReplaceAllStringFunc(value, RedactSecret)
	value = longTokenPattern.ReplaceAllStringFunc(value, func(token string) string {
		if strings.Contains(token, ".") || strings.Contains(token, "/") {
			return token
		}
		return RedactSecret(token)
	})
	return value
}
