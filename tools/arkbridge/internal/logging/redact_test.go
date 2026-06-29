package logging

import (
	"strings"
	"testing"
)

func TestRedactSecret(t *testing.T) {
	liveKey := "sk_" + "live_" + "abcdefghijklmnopqrstuvwxyz"
	testKey := "sk_" + "test_" + "not_a_real_key_for_redaction_unit_test"
	bearerToken := "Bearer " + "unit_test_bearer_token_1234567890"
	longToken := "unit_test_long_sensitive_token_1234567890"

	cases := []struct {
		name string
		in   string
		want string
	}{
		{name: "live key", in: liveKey, want: "sk_live_****wxyz"},
		{name: "test key", in: testKey, want: "sk_test_****test"},
		{name: "bearer token", in: bearerToken, want: "bearer_****7890"},
		{name: "long token", in: longToken, want: "bearer_****7890"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := RedactSecret(tc.in); got != tc.want {
				t.Fatalf("unexpected redact: got %q want %q", got, tc.want)
			}
		})
	}
}

func TestRedactText(t *testing.T) {
	liveKey := "sk_" + "live_" + "abcdefghijklmnopqrstuvwxyz"
	envKey := "unit_test_env_key_12345678901234567890"
	bearerValue := "unit_test_bearer_token_12345678901234567890"
	longToken := "unit_test_long_opaque_token_12345678901234567890"
	input := "Authorization: Bearer " + bearerValue +
		" BYTEPLUS_ARK_API_KEY=" + envKey +
		" ARK_API_KEY=" + envKey +
		" provider=" + liveKey +
		" token=" + longToken

	got := RedactText(input)
	for _, leaked := range []string{bearerValue, envKey, liveKey, longToken} {
		if strings.Contains(got, leaked) {
			t.Fatalf("redacted text leaked %q in %q", leaked, got)
		}
	}
	for _, want := range []string{"Authorization: Bearer ****", "BYTEPLUS_ARK_API_KEY=****", "ARK_API_KEY=****", "sk_live_****wxyz"} {
		if !strings.Contains(got, want) {
			t.Fatalf("redacted text missing %q in %q", want, got)
		}
	}

	// Verify request ID is preserved
	reqIdInput := "Error: the API key or AK/SK in the request is missing or invalid. request id: 20260629083329bc0195d8200632bfa8"
	reqIdGot := RedactText(reqIdInput)
	expectedReqIdPart := "request id: 20260629083329bc0195d8200632bfa8"
	if !strings.Contains(reqIdGot, expectedReqIdPart) {
		t.Fatalf("request ID was incorrectly redacted: got %q, expected to contain %q", reqIdGot, expectedReqIdPart)
	}
}

