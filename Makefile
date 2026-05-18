# cvkit platform — top-level Makefile
# Provides unified build/test/deploy targets for all components.

.PHONY: all build test clean deploy-local deploy-local-down fmt lint

# Default: build everything
all: build

## Build all components
build: build-emd build-autotune build-postprocess

build-emd:
	cmake -S components/emd -B components/emd/build -DCMAKE_BUILD_TYPE=Release
	cmake --build components/emd/build --parallel

build-autotune:
	cd components/autotune && go build ./...

build-postprocess:
	cd components/postprocess && go build ./...

## Run all tests
test: test-emd test-autotune test-postprocess

test-emd:
	cd components/emd/build && ctest --output-on-failure

test-autotune:
	cd components/autotune && go test ./...

test-postprocess:
	cd components/postprocess && go test ./...

## Local dev stack (requires Docker)
deploy-local:
	docker compose -f deploy/docker/docker-compose.yml up --build

deploy-local-down:
	docker compose -f deploy/docker/docker-compose.yml down

## Format Go code
fmt:
	cd components/autotune && go fmt ./...
	cd components/postprocess && go fmt ./...

## Lint Go code (requires golangci-lint)
lint:
	cd components/autotune && golangci-lint run ./...
	cd components/postprocess && golangci-lint run ./...

clean:
	rm -rf components/emd/build
	cd components/autotune && go clean
	cd components/postprocess && go clean
