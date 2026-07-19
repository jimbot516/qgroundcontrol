# MCP Agent Control

QGroundControl can expose an opt-in local control bridge to a Model Context Protocol (MCP) server. This lets
agents use QGC's vehicle and mission APIs directly instead of driving the user interface.

The MCP adapter and complete setup instructions are in
[`tools/mcp/README.md`](https://github.com/mavlink/qgroundcontrol/blob/master/tools/mcp/README.md).

The bridge is disabled by default, binds only to `127.0.0.1`, and requires a bearer token with at least 32
characters. Start QGC with the token in its environment:

```bash
export QGC_MCP_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
./QGroundControl --mcp-control
```

The companion stdio server is `tools/mcp/qgc_mcp_server.py`. Configure an MCP client to launch that script
with the same `QGC_MCP_TOKEN`.

Live-flight actions and plan uploads require an explicit `confirm: true` argument. Plan editing tools operate
on an isolated QGC `PlanMasterController` and do not change the vehicle until the upload tool is called.

The adapter also exposes bounded state waiting, preflight and sensor health, recent terminal MAVLink command
acknowledgements, explicit mission/geofence/rally download, and plan validation. Vehicle commands return a
`command_status_after_sequence` watermark for querying acknowledgements that arrive after the request. Agents
should use `qgc_wait_for_state` to verify asynchronous outcomes such as arming, reaching altitude, landing, and
plan synchronization instead of assuming that an accepted command has completed.
