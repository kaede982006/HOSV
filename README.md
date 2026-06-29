# HOSV: Native GUI Client for BytePlus ModelArk Video Generation

HOSV is a Linux/X11 native C++ application for AI video generation. Runtime video generation now uses the official BytePlus ModelArk provider through a small Go bridge binary named `arkbridge`.

The previous unofficial `seedance2.ai` provider is disabled for runtime use. Use `--provider byteplus-ark`.

## Project Structure

- `HOSVMain/`: C++ application layer, native X11 UI, job workflow, downloads, and provider orchestration.
- `tools/arkbridge/`: Go bridge for official BytePlus ModelArk API calls.
- `HOSVApi/`: legacy SDK code retained for source compatibility only. It is not used by the app runtime.
- `fonts/`, `logo.png`, `build.sh`: runtime assets and build helper.

## BytePlus Provider

The default provider is `byteplus-ark`.

`arkbridge` reads secrets only from environment variables, receives non-secret generation parameters as JSON over stdin, calls official BytePlus ModelArk endpoints, and returns normalized JSON on stdout.

When an API key is entered in the GUI, HOSV temporarily maps it to `BYTEPLUS_ARK_API_KEY` for the active job and restores the previous process environment value when that job exits. The key is not written to status text, bridge stdin, stdout, or stderr.

Default official endpoint:

```text
https://ark.ap-southeast.bytepluses.com/api/v3
```

Default models:

```text
BYTEPLUS_ARK_MODEL=dreamina-seedance-2-0-260128
BYTEPLUS_ARK_FAST_MODEL=dreamina-seedance-2-0-fast-260128
```

These defaults are based on BytePlus ModelArk API Explorer / docs for content generation tasks. If your BytePlus account exposes a different model ID or region, set the environment variables below.

## Environment

Required:

```bash
export BYTEPLUS_ARK_API_KEY="ark-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx-xxxxx"
```

Fallback:

```bash
export ARK_API_KEY="..."
```

Optional:

```bash
export BYTEPLUS_ARK_BASE_URL="https://ark.ap-southeast.bytepluses.com/api/v3"
export BYTEPLUS_ARK_MODEL="dreamina-seedance-2-0-260128"
export BYTEPLUS_ARK_FAST_MODEL="dreamina-seedance-2-0-fast-260128"
export BYTEPLUS_ARK_REGION="ap-southeast"
export BYTEPLUS_ARK_TIMEOUT_MS=120000
export BYTEPLUS_ARK_POLL_INITIAL_MS=3000
export BYTEPLUS_ARK_POLL_MAX_MS=15000
export BYTEPLUS_ARK_POLL_TIMEOUT_MS=900000
export BYTEPLUS_ARK_DEBUG=false
export BYTEPLUS_ARK_BRIDGE_PATH="./arkbridge"
```

The GUI can attach local image paths through the file browser. `arkbridge` converts readable local media files to inline `data:` URLs before calling BytePlus. Public or signed `http://` / `https://` media URLs are still preferred for large files.

## Build

Install dependencies:

```bash
sudo apt update
sudo apt install -y cmake g++ golang-go
```

Build everything:

```bash
./build.sh
```

Manual bridge build:

```bash
cd tools/arkbridge
go mod tidy
go build -o ../../build/arkbridge ./cmd/arkbridge
```

Manual C++ build:

```bash
cmake -S HOSVMain -B build/HOSV -DCMAKE_BUILD_TYPE=Release
cmake --build build/HOSV --parallel
```

## Run

```bash
export BYTEPLUS_ARK_API_KEY="ark-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx-xxxxx"
./dist/HOSV --provider byteplus-ark
```

Deprecated providers fail intentionally:

```bash
./dist/HOSV --provider seedance2.ai
```

## arkbridge CLI

All commands read JSON from stdin and write JSON to stdout:

```bash
./dist/arkbridge schema
echo '{"prompt":"A clean product film","duration_seconds":5}' | ./dist/arkbridge create
echo '{"task_id":"..."}' | ./dist/arkbridge get
echo '{"task_id":"..."}' | ./dist/arkbridge wait
echo '{"task_id":"..."}' | ./dist/arkbridge cancel
```

`arkbridge self-test` validates local configuration and does not spend credits. Without an API key it returns:

```json
{"ok":false,"error_code":"missing_api_key","error_message":"BYTEPLUS_ARK_API_KEY or ARK_API_KEY is required","retryable":false}
```

## Tests

Unit tests do not call real APIs and do not spend credits.

```bash
cd tools/arkbridge
go test ./...
go vet ./...

cd ../..
cmake -S HOSVMain -B build/HOSV -DCMAKE_BUILD_TYPE=Debug
cmake --build build/HOSV --parallel
./build/HOSV/hosv --self-test
```

Real API tests must be explicitly guarded by:

```bash
export BYTEPLUS_ARK_RUN_INTEGRATION_TESTS=1
```

No real credit-spending integration test is run by default.

`arkbridge schema` and unit tests are local-only checks. They do not call BytePlus generation endpoints and must not require a real API key.

## Troubleshooting

- `go: command not found`: install Go and rebuild.
- `missing_api_key`: set `BYTEPLUS_ARK_API_KEY` or `ARK_API_KEY`.
- `401` / `403`: verify the API key, account permissions, and region.
- `quota_exceeded` / `insufficient_balance`: check BytePlus quota and billing.
- `invalid_model`: set `BYTEPLUS_ARK_MODEL` to a model available in your ModelArk account.
- `invalid_media_url`: use public or signed media URLs, not local file paths.
- `media_not_accessible`: verify the URL is reachable by BytePlus.
- `polling_timeout`: increase `BYTEPLUS_ARK_POLL_TIMEOUT_MS`.

## License

This project is licensed under GPLv3. See `LICENSE`.
