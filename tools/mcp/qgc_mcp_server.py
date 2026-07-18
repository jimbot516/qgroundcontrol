#!/usr/bin/env python3
"""Model Context Protocol stdio adapter for QGroundControl's local bridge."""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

SERVER_NAME = "qgroundcontrol"
SERVER_VERSION = "0.1.0"
DEFAULT_URL = "http://127.0.0.1:49300"
LATEST_PROTOCOL_VERSION = "2025-11-25"
SUPPORTED_PROTOCOL_VERSIONS = {
    LATEST_PROTOCOL_VERSION,
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
}


class QgcError(RuntimeError):
    """Expected QGroundControl bridge error."""


def _object_schema(
    properties: dict[str, Any] | None = None,
    required: list[str] | None = None,
) -> dict[str, Any]:
    return {
        "type": "object",
        "properties": properties or {},
        "required": required or [],
        "additionalProperties": False,
    }


VEHICLE_ID = {
    "type": "integer",
    "minimum": 1,
    "maximum": 255,
    "description": "MAVLink vehicle ID. Omit to use QGC's active vehicle.",
}
CONFIRM = {
    "type": "boolean",
    "description": "Must be true to acknowledge that this action affects a vehicle or uploads a plan.",
}
LATITUDE = {"type": "number", "minimum": -90, "maximum": 90}
LONGITUDE = {"type": "number", "minimum": -180, "maximum": 180}
ALTITUDE = {
    "type": "number",
    "minimum": -1000,
    "maximum": 100000,
    "description": "Altitude in meters. Mission-item altitudes are relative to home by default.",
}


def _live_schema(
    properties: dict[str, Any] | None = None, required: list[str] | None = None
) -> dict[str, Any]:
    merged = {"vehicle_id": VEHICLE_ID, "confirm": CONFIRM}
    merged.update(properties or {})
    return _object_schema(merged, [*(required or []), "confirm"])


TOOLS: list[dict[str, Any]] = [
    {
        "name": "qgc_get_status",
        "description": "Read connected vehicles, active-vehicle flight state, location, and MCP plan state.",
        "inputSchema": _object_schema({"vehicle_id": VEHICLE_ID}),
    },
    {
        "name": "qgc_select_vehicle",
        "description": "Select which connected vehicle is active in QGroundControl.",
        "inputSchema": _object_schema({"vehicle_id": VEHICLE_ID}, ["vehicle_id"]),
    },
    {
        "name": "qgc_set_armed",
        "description": "Arm or disarm a vehicle. In-flight disarming is intentionally blocked.",
        "inputSchema": _live_schema({"armed": {"type": "boolean"}}, ["armed"]),
    },
    {
        "name": "qgc_takeoff",
        "description": "Command a connected vehicle to take off to a relative altitude. This moves the aircraft.",
        "inputSchema": _live_schema(
            {"altitude_m": {"type": "number", "exclusiveMinimum": 0, "maximum": 500}},
            ["altitude_m"],
        ),
    },
    {
        "name": "qgc_land",
        "description": "Command the vehicle to land at its current location. This moves the aircraft.",
        "inputSchema": _live_schema(),
    },
    {
        "name": "qgc_return_to_launch",
        "description": "Command return-to-launch. This moves the aircraft.",
        "inputSchema": _live_schema({"smart_rtl": {"type": "boolean", "default": False}}),
    },
    {
        "name": "qgc_pause",
        "description": "Pause/hold the active vehicle at its current location.",
        "inputSchema": _live_schema(),
    },
    {
        "name": "qgc_start_mission",
        "description": "Start the mission currently uploaded to the vehicle. This moves the aircraft.",
        "inputSchema": _live_schema(),
    },
    {
        "name": "qgc_goto",
        "description": "Send the flying vehicle to a latitude/longitude using QGC's guided-mode distance checks.",
        "inputSchema": _live_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE},
            ["latitude", "longitude"],
        ),
    },
    {
        "name": "qgc_set_roi",
        "description": "Point the vehicle/gimbal at a live region of interest coordinate.",
        "inputSchema": _live_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE, "altitude_m": ALTITUDE},
            ["latitude", "longitude"],
        ),
    },
    {
        "name": "qgc_clear_roi",
        "description": "Stop live region-of-interest tracking.",
        "inputSchema": _live_schema(),
    },
    {
        "name": "qgc_plan_get",
        "description": "Read the MCP bridge's complete editable QGC plan as QGC Plan JSON.",
        "inputSchema": _object_schema(),
    },
    {
        "name": "qgc_plan_clear",
        "description": "Clear the editable MCP plan in QGC. This does not change the vehicle until upload.",
        "inputSchema": _object_schema(),
    },
    {
        "name": "qgc_plan_load",
        "description": "Load an existing .plan, .waypoints, or mission text file into the editable MCP plan.",
        "inputSchema": _object_schema({"path": {"type": "string", "minLength": 1}}, ["path"]),
    },
    {
        "name": "qgc_plan_save",
        "description": "Save the editable MCP plan to a local .plan file.",
        "inputSchema": _object_schema({"path": {"type": "string", "minLength": 1}}, ["path"]),
    },
    {
        "name": "qgc_plan_add_waypoint",
        "description": "Append a waypoint to the editable plan. This does not change the vehicle until upload.",
        "inputSchema": _object_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE, "altitude_m": ALTITUDE},
            ["latitude", "longitude", "altitude_m"],
        ),
    },
    {
        "name": "qgc_plan_add_takeoff",
        "description": "Append a takeoff mission item. This does not change the vehicle until upload.",
        "inputSchema": _object_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE, "altitude_m": ALTITUDE},
            ["latitude", "longitude", "altitude_m"],
        ),
    },
    {
        "name": "qgc_plan_add_roi",
        "description": "Append a region-of-interest mission item. This does not change the vehicle until upload.",
        "inputSchema": _object_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE, "altitude_m": ALTITUDE},
            ["latitude", "longitude", "altitude_m"],
        ),
    },
    {
        "name": "qgc_plan_add_rally_point",
        "description": "Append a rally point to the editable plan. This does not change the vehicle until upload.",
        "inputSchema": _object_schema(
            {"latitude": LATITUDE, "longitude": LONGITUDE, "altitude_m": ALTITUDE},
            ["latitude", "longitude", "altitude_m"],
        ),
    },
    {
        "name": "qgc_plan_upload",
        "description": "Upload the complete editable plan, including mission and rally points, to the active vehicle.",
        "inputSchema": _object_schema(
            {
                "confirm": CONFIRM,
                "allow_firmware_mismatch": {
                    "type": "boolean",
                    "default": False,
                    "description": "Override QGC's firmware/vehicle plan mismatch check.",
                },
            },
            ["confirm"],
        ),
    },
]

ACTION_NAMES = {
    "qgc_get_status": "get_status",
    "qgc_select_vehicle": "select_vehicle",
    "qgc_set_armed": "vehicle_set_armed",
    "qgc_takeoff": "vehicle_takeoff",
    "qgc_land": "vehicle_land",
    "qgc_return_to_launch": "vehicle_rtl",
    "qgc_pause": "vehicle_pause",
    "qgc_start_mission": "vehicle_start_mission",
    "qgc_goto": "vehicle_goto",
    "qgc_set_roi": "vehicle_set_roi",
    "qgc_clear_roi": "vehicle_clear_roi",
    "qgc_plan_get": "plan_get",
    "qgc_plan_clear": "plan_clear",
    "qgc_plan_load": "plan_load",
    "qgc_plan_save": "plan_save",
    "qgc_plan_add_waypoint": "plan_add_waypoint",
    "qgc_plan_add_takeoff": "plan_add_takeoff",
    "qgc_plan_add_roi": "plan_add_roi",
    "qgc_plan_add_rally_point": "plan_add_rally_point",
    "qgc_plan_upload": "plan_upload",
}


class QgcClient:
    def __init__(self, base_url: str, token: str) -> None:
        if len(token) < 32:
            raise QgcError("QGC_MCP_TOKEN must contain at least 32 characters")
        parsed_url = urllib.parse.urlparse(base_url)
        if parsed_url.scheme != "http" or parsed_url.hostname not in {
            "127.0.0.1",
            "localhost",
            "::1",
        }:
            raise QgcError("QGC_MCP_URL must be an HTTP loopback URL")
        self._base_url = base_url.rstrip("/")
        self._token = token

    def call(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        body = json.dumps({"name": name, "arguments": arguments}, separators=(",", ":")).encode()
        request = urllib.request.Request(
            f"{self._base_url}/v1/tools/call",
            data=body,
            headers={
                "Authorization": f"Bearer {self._token}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                payload = json.load(response)
        except urllib.error.HTTPError as error:
            try:
                payload = json.load(error)
                message = payload.get("error", str(error))
            except (json.JSONDecodeError, UnicodeDecodeError, AttributeError):
                message = str(error)
            raise QgcError(message) from error
        except urllib.error.URLError as error:
            raise QgcError(
                f"Cannot reach QGroundControl at {self._base_url}. "
                "Start QGC with --mcp-control and the same QGC_MCP_TOKEN."
            ) from error
        except (json.JSONDecodeError, UnicodeDecodeError, AttributeError) as error:
            raise QgcError("QGroundControl returned an invalid JSON response") from error

        if not isinstance(payload, dict) or not payload.get("ok"):
            message = (
                payload.get("error", "Invalid response from QGroundControl")
                if isinstance(payload, dict)
                else "Invalid response"
            )
            raise QgcError(message)
        result = payload.get("result", {})
        return result if isinstance(result, dict) else {"value": result}


def handle_request(message: dict[str, Any], client: QgcClient) -> dict[str, Any] | None:
    request_id = message.get("id")
    method = message.get("method")
    if request_id is None:
        return None

    response: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id}
    if message.get("jsonrpc") != "2.0" or not isinstance(method, str):
        response["error"] = {"code": -32600, "message": "Invalid JSON-RPC request"}
        return response

    params = message.get("params", {})
    if not isinstance(params, dict):
        response["error"] = {"code": -32602, "message": "params must be an object"}
        return response

    try:
        if method == "initialize":
            requested_version = params.get("protocolVersion")
            protocol_version = (
                requested_version
                if requested_version in SUPPORTED_PROTOCOL_VERSIONS
                else LATEST_PROTOCOL_VERSION
            )
            response["result"] = {
                "protocolVersion": protocol_version,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": (
                    "Use qgc_get_status before live actions. Live-flight and upload tools require confirm=true. "
                    "Plan edits are local to QGC until qgc_plan_upload is called."
                ),
            }
        elif method == "ping":
            response["result"] = {}
        elif method == "tools/list":
            response["result"] = {"tools": TOOLS}
        elif method == "tools/call":
            tool_name = params.get("name", "")
            arguments = params.get("arguments", {})
            if tool_name not in ACTION_NAMES:
                response["error"] = {"code": -32602, "message": f"Unknown tool: {tool_name}"}
                return response
            if not isinstance(arguments, dict):
                response["error"] = {"code": -32602, "message": "Tool arguments must be an object"}
                return response
            result = client.call(ACTION_NAMES[tool_name], arguments)
            response["result"] = {
                "content": [{"type": "text", "text": json.dumps(result, indent=2, sort_keys=True)}],
                "structuredContent": result,
                "isError": False,
            }
        else:
            response["error"] = {"code": -32601, "message": f"Method not found: {method}"}
    except QgcError as error:
        response["result"] = {
            "content": [{"type": "text", "text": str(error)}],
            "isError": True,
        }
    return response


def main() -> int:
    try:
        client = QgcClient(
            os.environ.get("QGC_MCP_URL", DEFAULT_URL),
            os.environ.get("QGC_MCP_TOKEN", ""),
        )
    except QgcError as error:
        print(error, file=sys.stderr)
        return 2

    for line in sys.stdin:
        try:
            message = json.loads(line)
            if not isinstance(message, dict):
                raise json.JSONDecodeError("message must be an object", line, 0)
            response = handle_request(message, client)
        except json.JSONDecodeError as error:
            response = {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32700, "message": f"Parse error: {error}"},
            }
        if response is not None:
            print(json.dumps(response, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
