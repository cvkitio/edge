# Gemini CLI: cvkit edge-motion-detector

This project is the `edge-motion-detector` (EMD) component of the cvkit platform.

## Project Structure

- `components/emd`: Core C-based motion detection and RTSP processing.
- `components/autotune`: Go-based parameter optimization.
- `components/postprocess`: Go-based event processing pipeline.
- `deploy/`: Kubernetes and Docker Compose deployment configurations.
- `docs/`: Architectural and API documentation.

## Build and Test

The project uses a top-level `Makefile` for unified operations.

- `make build`: Builds all components.
- `make test`: Runs tests for all components.
- `make fmt`: Formats Go code.
- `make lint`: Lints Go code.

## GitHub Automation

The following workflows are configured:
- `CI`: Standard build and test on push/PR.
- `Gemini Dispatch`: Routes Gemini CLI commands from comments.
- `Gemini Review`: Automatic PR review using Gemini (triggered on PR open and every new push).
- `Gemini Triage`: Automatic issue triaging and labeling.
- `Gemini Invoke`: Interactive command execution.
- `Gemini Plan & Execute`: Executes approved plans for features/fixes.

## Required Secrets/Variables for Gemini CLI

To enable Gemini CLI automation, ensure the following are configured in GitHub Actions:

### Secrets
- `GEMINI_API_KEY`: API key for Google Gemini.
- `APP_PRIVATE_KEY`: (Optional) Private key for a GitHub App identity.

### Variables
- `GOOGLE_CLOUD_PROJECT`: GCP Project ID.
- `GOOGLE_CLOUD_LOCATION`: GCP Location (e.g., `us-central1`).
- `SERVICE_ACCOUNT_EMAIL`: Service account for GCP authentication.
- `GCP_WIF_PROVIDER`: Workload Identity Provider string.
- `GEMINI_MODEL`: Model name to use (e.g., `gemini-1.5-pro`).
- `APP_ID`: (Optional) GitHub App ID.
