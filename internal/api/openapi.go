package api

// openAPISpec is the OpenAPI 3.0 specification for the EMD Agent API.
const openAPISpec = `{
  "openapi": "3.0.0",
  "info": {
    "title": "EMD Agent API",
    "version": "1.0.0",
    "description": "Edge Motion Detector Agent REST API for runtime configuration, clip management, and monitoring",
    "contact": {
      "name": "EMD Agent",
      "url": "https://github.com/cvkitio/cvkit"
    }
  },
  "servers": [
    {
      "url": "http://localhost:8080",
      "description": "Local development server"
    }
  ],
  "tags": [
    {
      "name": "Health",
      "description": "Health check endpoints"
    },
    {
      "name": "Cameras",
      "description": "Camera configuration and management"
    },
    {
      "name": "Clips",
      "description": "Video clip browsing and playback"
    }
  ],
  "paths": {
    "/health": {
      "get": {
        "tags": ["Health"],
        "summary": "Health check",
        "description": "Returns the health status of the agent",
        "operationId": "getHealth",
        "responses": {
          "200": {
            "description": "Agent is healthy",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {
                      "type": "string",
                      "example": "ok"
                    }
                  }
                }
              }
            }
          }
        }
      }
    },
    "/api/cameras": {
      "get": {
        "tags": ["Cameras"],
        "summary": "List cameras",
        "description": "Returns a list of all configured cameras",
        "operationId": "listCameras",
        "responses": {
          "200": {
            "description": "List of camera names",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "cameras": {
                      "type": "array",
                      "items": {
                        "type": "string"
                      },
                      "example": ["axis_81_1", "axis_82_2"]
                    }
                  }
                }
              }
            }
          }
        }
      }
    },
    "/api/cameras/{name}/config": {
      "get": {
        "tags": ["Cameras"],
        "summary": "Get camera configuration",
        "description": "Returns the current inspector configuration for a camera",
        "operationId": "getCameraConfig",
        "parameters": [
          {
            "name": "name",
            "in": "path",
            "required": true,
            "description": "Camera name",
            "schema": {
              "type": "string"
            }
          }
        ],
        "responses": {
          "200": {
            "description": "Camera configuration",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/InspectorConfig"
                }
              }
            }
          },
          "404": {
            "description": "Camera not found",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          }
        }
      },
      "put": {
        "tags": ["Cameras"],
        "summary": "Update camera configuration",
        "description": "Updates the inspector configuration for a camera at runtime",
        "operationId": "updateCameraConfig",
        "parameters": [
          {
            "name": "name",
            "in": "path",
            "required": true,
            "description": "Camera name",
            "schema": {
              "type": "string"
            }
          }
        ],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/InspectorConfigUpdate"
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Configuration updated successfully",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Success"
                }
              }
            }
          },
          "400": {
            "description": "Invalid configuration",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          },
          "404": {
            "description": "Camera not found",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          }
        }
      }
    },
    "/api/clips": {
      "get": {
        "tags": ["Clips"],
        "summary": "List clips",
        "description": "Returns a list of all recorded video clips",
        "operationId": "listClips",
        "parameters": [
          {
            "name": "camera",
            "in": "query",
            "required": false,
            "description": "Filter clips by camera name",
            "schema": {
              "type": "string"
            }
          }
        ],
        "responses": {
          "200": {
            "description": "List of clips",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "clips": {
                      "type": "array",
                      "items": {
                        "$ref": "#/components/schemas/ClipInfo"
                      }
                    },
                    "total": {
                      "type": "integer",
                      "description": "Total number of clips",
                      "example": 42
                    }
                  }
                }
              }
            }
          },
          "500": {
            "description": "Failed to scan clips",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          }
        }
      }
    },
    "/api/clips/{camera}/{filename}": {
      "get": {
        "tags": ["Clips"],
        "summary": "Stream clip",
        "description": "Serves a video clip file with HTTP Range support for streaming",
        "operationId": "streamClip",
        "parameters": [
          {
            "name": "camera",
            "in": "path",
            "required": true,
            "description": "Camera name",
            "schema": {
              "type": "string"
            }
          },
          {
            "name": "filename",
            "in": "path",
            "required": true,
            "description": "Clip filename",
            "schema": {
              "type": "string"
            }
          }
        ],
        "responses": {
          "200": {
            "description": "Video clip file",
            "headers": {
              "Accept-Ranges": {
                "schema": {
                  "type": "string",
                  "example": "bytes"
                },
                "description": "Indicates range requests are supported"
              },
              "Cache-Control": {
                "schema": {
                  "type": "string",
                  "example": "public, max-age=3600"
                }
              }
            },
            "content": {
              "video/mp2t": {
                "schema": {
                  "type": "string",
                  "format": "binary"
                }
              },
              "video/mp4": {
                "schema": {
                  "type": "string",
                  "format": "binary"
                }
              },
              "video/x-matroska": {
                "schema": {
                  "type": "string",
                  "format": "binary"
                }
              }
            }
          },
          "400": {
            "description": "Invalid path",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          },
          "403": {
            "description": "Access denied (path traversal attempt)",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          },
          "404": {
            "description": "Clip not found",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Error"
                }
              }
            }
          }
        }
      }
    }
  },
  "components": {
    "schemas": {
      "InspectorConfig": {
        "type": "object",
        "description": "Motion detection inspector configuration",
        "properties": {
          "motion_z_high": {
            "type": "number",
            "format": "float",
            "description": "Z-score threshold for motion detection (higher = less sensitive)",
            "minimum": 0,
            "maximum": 100,
            "example": 3.0
          },
          "intra_ratio_high": {
            "type": "number",
            "format": "float",
            "description": "Intra-macroblock ratio threshold",
            "minimum": 0,
            "maximum": 100,
            "example": 2.5
          },
          "on_threshold": {
            "type": "integer",
            "description": "Consecutive frames above threshold to trigger motion",
            "minimum": 1,
            "maximum": 255,
            "example": 2
          },
          "off_threshold": {
            "type": "integer",
            "description": "Consecutive frames below threshold to return to idle",
            "minimum": 1,
            "maximum": 255,
            "example": 45
          },
          "bpf_floor": {
            "type": "number",
            "format": "float",
            "description": "Minimum bytes-per-frame (prevents division by zero)",
            "minimum": 0,
            "example": 100.0
          },
          "configured_periodic_kf": {
            "type": "boolean",
            "description": "Whether camera sends periodic keyframes",
            "example": false
          },
          "gradual_enabled": {
            "type": "boolean",
            "description": "Enable gradual scene change detection",
            "example": false
          },
          "gradual_threshold": {
            "type": "number",
            "format": "float",
            "description": "Gradual change threshold",
            "minimum": 0,
            "maximum": 1,
            "example": 0.15
          },
          "gradual_window_frames": {
            "type": "integer",
            "description": "Gradual detection window size in frames",
            "minimum": 1,
            "example": 900
          }
        }
      },
      "InspectorConfigUpdate": {
        "type": "object",
        "description": "Partial inspector configuration update (all fields optional)",
        "properties": {
          "motion_z_high": {
            "type": "number",
            "format": "float",
            "minimum": 0,
            "maximum": 100
          },
          "intra_ratio_high": {
            "type": "number",
            "format": "float",
            "minimum": 0,
            "maximum": 100
          },
          "on_threshold": {
            "type": "integer",
            "minimum": 1,
            "maximum": 255
          },
          "off_threshold": {
            "type": "integer",
            "minimum": 1,
            "maximum": 255
          },
          "bpf_floor": {
            "type": "number",
            "format": "float",
            "minimum": 0
          },
          "configured_periodic_kf": {
            "type": "boolean"
          },
          "gradual_enabled": {
            "type": "boolean"
          },
          "gradual_threshold": {
            "type": "number",
            "format": "float",
            "minimum": 0,
            "maximum": 1
          },
          "gradual_window_frames": {
            "type": "integer",
            "minimum": 1
          }
        }
      },
      "ClipInfo": {
        "type": "object",
        "description": "Video clip metadata",
        "properties": {
          "camera": {
            "type": "string",
            "description": "Camera name",
            "example": "axis_82_2"
          },
          "filename": {
            "type": "string",
            "description": "Clip filename",
            "example": "20260518_091523_motion.ts"
          },
          "path": {
            "type": "string",
            "description": "Full file path on server",
            "example": "/var/lib/emd-agent/clips/axis_82_2/20260518_091523_motion.ts"
          },
          "size": {
            "type": "integer",
            "description": "File size in bytes",
            "example": 2456789
          },
          "mod_time": {
            "type": "string",
            "format": "date-time",
            "description": "Last modification timestamp",
            "example": "2026-05-18T09:15:23Z"
          },
          "url": {
            "type": "string",
            "description": "URL to stream the clip",
            "example": "/api/clips/axis_82_2/20260518_091523_motion.ts"
          }
        }
      },
      "Error": {
        "type": "object",
        "description": "Error response",
        "properties": {
          "error": {
            "type": "string",
            "description": "Error message",
            "example": "camera not found"
          }
        }
      },
      "Success": {
        "type": "object",
        "description": "Success response",
        "properties": {
          "success": {
            "type": "boolean",
            "example": true
          },
          "message": {
            "type": "string",
            "example": "Configuration updated for camera axis_82_2"
          }
        }
      }
    }
  }
}
`

// swaggerUIHTML is the embedded Swagger UI for browsing the API documentation.
const swaggerUIHTML = `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>EMD Agent API - Documentation</title>
    <link rel="stylesheet" type="text/css" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css">
    <style>
        html {
            box-sizing: border-box;
            overflow: -moz-scrollbars-vertical;
            overflow-y: scroll;
        }
        *, *:before, *:after {
            box-sizing: inherit;
        }
        body {
            margin: 0;
            padding: 0;
            background: #0f172a;
        }
        .swagger-ui .topbar {
            background-color: #1e293b;
            border-bottom: 2px solid #334155;
        }
        .swagger-ui .topbar .download-url-wrapper .download-url-button {
            background: #3b82f6;
        }
        .swagger-ui .info .title {
            color: #60a5fa;
        }
        .swagger-ui .scheme-container {
            background: #1e293b;
            box-shadow: 0 1px 2px 0 rgba(0,0,0,0.25);
        }
    </style>
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-standalone-preset.js"></script>
    <script>
        window.onload = function() {
            window.ui = SwaggerUIBundle({
                url: "/docs/openapi.json",
                dom_id: '#swagger-ui',
                deepLinking: true,
                presets: [
                    SwaggerUIBundle.presets.apis,
                    SwaggerUIStandalonePreset
                ],
                plugins: [
                    SwaggerUIBundle.plugins.DownloadUrl
                ],
                layout: "StandaloneLayout",
                defaultModelsExpandDepth: 1,
                defaultModelExpandDepth: 1,
                docExpansion: "list",
                filter: true,
                tryItOutEnabled: true
            });
        };
    </script>
</body>
</html>
`
