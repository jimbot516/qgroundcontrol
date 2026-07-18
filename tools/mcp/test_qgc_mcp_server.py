from __future__ import annotations

import io
import json
import unittest
from unittest.mock import patch

from qgc_mcp_server import (
    ACTION_NAMES,
    LATEST_PROTOCOL_VERSION,
    TOOLS,
    QgcClient,
    QgcError,
    handle_request,
)

TOKEN = "a" * 32


class _Response(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()


class QgcMcpServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.client = QgcClient("http://127.0.0.1:49300", TOKEN)

    def test_short_token_is_rejected(self) -> None:
        with self.assertRaisesRegex(QgcError, "at least 32"):
            QgcClient("http://127.0.0.1:49300", "short")

    def test_non_loopback_url_is_rejected(self) -> None:
        with self.assertRaisesRegex(QgcError, "loopback"):
            QgcClient("https://example.com", TOKEN)

    def test_initialize_advertises_tools(self) -> None:
        response = handle_request(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-06-18"},
            },
            self.client,
        )
        assert response is not None
        self.assertEqual(response["result"]["protocolVersion"], "2025-06-18")
        self.assertIn("tools", response["result"]["capabilities"])

    def test_initialize_negotiates_latest_for_unknown_version(self) -> None:
        response = handle_request(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2099-01-01"},
            },
            self.client,
        )
        assert response is not None
        self.assertEqual(response["result"]["protocolVersion"], LATEST_PROTOCOL_VERSION)

    def test_every_tool_has_an_action(self) -> None:
        self.assertEqual({tool["name"] for tool in TOOLS}, set(ACTION_NAMES))

    @patch("urllib.request.urlopen")
    def test_tool_call_forwards_to_bridge(self, urlopen) -> None:
        urlopen.return_value = _Response(
            json.dumps({"ok": True, "result": {"armed": False}}).encode()
        )
        response = handle_request(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": "qgc_get_status", "arguments": {}},
            },
            self.client,
        )
        assert response is not None
        self.assertFalse(response["result"]["isError"])
        self.assertEqual(response["result"]["structuredContent"], {"armed": False})
        request = urlopen.call_args.args[0]
        self.assertEqual(request.get_header("Authorization"), f"Bearer {TOKEN}")

    def test_unknown_tool_is_protocol_error(self) -> None:
        response = handle_request(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "qgc_does_not_exist", "arguments": {}},
            },
            self.client,
        )
        assert response is not None
        self.assertEqual(response["error"]["code"], -32602)

    def test_malformed_params_are_protocol_error(self) -> None:
        response = handle_request(
            {"jsonrpc": "2.0", "id": 4, "method": "tools/list", "params": []},
            self.client,
        )
        assert response is not None
        self.assertEqual(response["error"]["code"], -32602)

    def test_notifications_do_not_receive_responses(self) -> None:
        self.assertIsNone(
            handle_request({"jsonrpc": "2.0", "method": "notifications/initialized"}, self.client)
        )


if __name__ == "__main__":
    unittest.main()
