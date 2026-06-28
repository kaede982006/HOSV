package byteplus

import "strings"

func NormalizeStatus(value string) string {
	normalized := strings.ToLower(strings.TrimSpace(value))
	switch normalized {
	case "queued", "queueing":
		return "queued"
	case "pending", "submitted", "created":
		return "pending"
	case "running":
		return "running"
	case "processing", "in_progress", "generating":
		return "processing"
	case "succeeded", "success":
		return "succeeded"
	case "completed", "complete", "done":
		return "completed"
	case "failed", "error":
		return "failed"
	case "cancelled", "canceled":
		return "cancelled"
	case "expired":
		return "expired"
	case "timeout", "timed_out":
		return "timeout"
	default:
		return "unknown"
	}
}

func IsTerminal(status string) bool {
	switch NormalizeStatus(status) {
	case "succeeded", "completed", "failed", "cancelled", "expired", "timeout":
		return true
	default:
		return false
	}
}
