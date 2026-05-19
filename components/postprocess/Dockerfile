FROM golang:1.23-bookworm AS builder
WORKDIR /build
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o postprocess ./cmd/postprocess

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
RUN groupadd -r postprocess && useradd -r -g postprocess postprocess
COPY --from=builder /build/postprocess /usr/local/bin/postprocess
USER postprocess
ENTRYPOINT ["/usr/local/bin/postprocess"]
