package logging

import "testing"

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
