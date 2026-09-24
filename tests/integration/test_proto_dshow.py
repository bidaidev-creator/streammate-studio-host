#!/usr/bin/env python3
"""PROTOTYPE (streammate-pivot#37): no-camera packaged Windows smoke."""
import json
import tempfile
import threading
import time

from test_host_lifecycle import rpc, start_host, stop_process, websocket_connect


def main():
    with tempfile.TemporaryDirectory(prefix="dshow-proto-") as home:
        process, port, lines = start_host(env={"STREAMMATE_HOME": home})
        for line in lines:
            print(line, flush=True)
        def drain():
            for line in process.stdout:
                print(line.rstrip(), flush=True)
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        sock = None
        counter = 0
        def call(method, params=None):
            nonlocal counter
            counter += 1
            response = rpc(sock, counter, method, params)
            print(json.dumps(response, ensure_ascii=False), flush=True)
            assert "error" not in response, response
            return response["result"]
        try:
            sock = websocket_connect(port)
            call("proto.createInput", {"sourceId": "cam", "kind": "dshow_input"})
            props = call("proto.props", {"sourceId": "cam"})
            by_name = {p["name"]: p for p in props["properties"]}
            assert by_name["video_device_id"]["type"] == "list"
            devices = by_name["video_device_id"]["items"]
            print(f"Observed {len(devices)} device entries; an empty list and no dialogs are expected on CI.")
            for button in ("video_config", "xbar_config"):
                assert by_name[button]["type"] == "button"
                pressed = call("proto.press", {"sourceId": "cam", "property": button})
                assert pressed["clicked"] is True
                assert pressed["hostThreadId"] > 0 and pressed["mainThreadId"] > 0
                print(f"{button}: host thread {pressed['hostThreadId']}, main thread {pressed['mainThreadId']}")
                time.sleep(1)  # Let the source thread consume its queued action.
                assert call("host.health")["status"] == "ready"
            call("host.shutdown")
            assert process.wait(timeout=15) == 0
            reader.join(timeout=2)
        finally:
            if sock is not None:
                sock.close()
            stop_process(process)


if __name__ == "__main__":
    main()
