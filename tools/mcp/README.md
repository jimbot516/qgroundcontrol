# QGroundControl MCP Control

This directory contains a dependency-free Model Context Protocol (MCP) stdio server that lets an agent use
QGroundControl's native vehicle and mission APIs without UI automation.

## Architecture

The feature has two local processes:

1. QGroundControl exposes an authenticated HTTP bridge on `127.0.0.1` when explicitly enabled.
2. `qgc_mcp_server.py` translates MCP stdio tool calls into requests to that bridge.

The bridge delegates commands to `Vehicle`, `FirmwarePlugin`, and `PlanMasterController`, preserving QGC's
firmware abstraction, guided-mode distance limits, plan validation, and active-mission upload checks.

## Start QGroundControl

Generate a token of at least 32 characters and provide it to QGC:

```bash
export QGC_MCP_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
./QGroundControl --mcp-control
```

The default port is `49300`. Use `--mcp-control-port 49301` and set `QGC_MCP_URL` on the MCP server if the
default is unavailable.

QGC refuses to start the bridge without a sufficiently long token. The listener is always loopback-only.

## Configure an MCP client

Point the client at the script and provide the same token. For clients using JSON configuration:

```json
{
  "mcpServers": {
    "qgroundcontrol": {
      "command": "python3",
      "args": ["/absolute/path/to/qgroundcontrol/tools/mcp/qgc_mcp_server.py"],
      "env": {
        "QGC_MCP_TOKEN": "replace-with-the-same-token"
      }
    }
  }
}
```

For clients using TOML configuration:

```toml
[mcp_servers.qgroundcontrol]
command = "python3"
args = ["/absolute/path/to/qgroundcontrol/tools/mcp/qgc_mcp_server.py"]
env = { QGC_MCP_TOKEN = "replace-with-the-same-token" }
```

Set `QGC_MCP_URL` in the MCP server environment only when using a non-default loopback port.

## Tools

The server exposes tools for:

- reading vehicle and editable-plan state;
- selecting the active vehicle;
- arming/disarming, takeoff, land, RTL, pause, and starting a mission;
- guided go-to and live ROI control;
- loading, saving, clearing, and inspecting QGC plans;
- appending takeoff, waypoint, ROI, and rally-point items;
- uploading the complete plan to the active vehicle.

Live-flight commands and plan upload require `confirm: true`. In-flight disarming is not exposed. Plan edits
remain local until `qgc_plan_upload` is explicitly called.

## Test

```bash
python3 -m unittest discover -s tools/mcp -p 'test_*.py'
```
