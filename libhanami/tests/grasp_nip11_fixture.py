"""Local GRASP-01 NIP-11 responses shared by push/fetch fixtures."""
import json

FAILURES = ("nip11-missing", "nip11-unsupported", "nip11-http-error",
            "nip11-disconnect", "nip11-malformed", "nip11-wrong-array",
            "nip11-wrong-item", "nip11-wrong-criteria", "nip11-wrong-curation",
            "nip11-no-criteria", "nip11-oversize", "nip11-http-oversize")


def serve_nip11(handler, request, headers):
    if request[1] != "/":
        return False
    assert request[0] == "GET"
    assert headers.get("accept") == "application/nostr+json"
    server = handler.server
    with server.lock:
        server.nip11_requests += 1
    case = server.case
    if case == "nip11-disconnect":
        return True  # End the connection without an HTTP response.
    doc = {"supported_grasps": ["GRASP-01", "GRASP-05"],
           "repo_acceptance_criteria": 'Local repositories only; ask "why".',
           "curation": "Fixture whitelist"}
    if case == "nip11-missing":
        del doc["supported_grasps"]
    elif case == "nip11-unsupported":
        doc["supported_grasps"] = ["GRASP-05"]
    elif case == "nip11-wrong-array":
        doc["supported_grasps"] = "GRASP-01"
    elif case == "nip11-wrong-item":
        doc["supported_grasps"] = ["GRASP-01", 5]
    elif case == "nip11-wrong-criteria":
        doc["repo_acceptance_criteria"] = 42
    elif case == "nip11-wrong-curation":
        doc["curation"] = []
    elif case == "nip11-no-criteria":
        del doc["repo_acceptance_criteria"]
    elif case == "nip11-no-curation":
        del doc["curation"]
        doc["supported_grasps"] = ["GRASP-01"]
    payload = json.dumps(doc).encode()
    if case == "nip11-malformed":
        payload = b'{"supported_grasps": ["GRASP-01"], broken'
    elif case in ("nip11-oversize", "nip11-http-oversize"):
        payload = b" " * (1024 * 1024 + 1)
    status = b"503 Unavailable" if case in ("nip11-http-error", "nip11-http-oversize") else b"200 OK"
    try:
        handler.wfile.write(b"HTTP/1.1 " + status + b"\r\nContent-Type: application/nostr+json\r\n"
                            b"Content-Length: " + str(len(payload)).encode() +
                            b"\r\nConnection: close\r\n\r\n" + payload)
        handler.wfile.flush()
    except (BrokenPipeError, ConnectionResetError):
        assert case in ("nip11-oversize", "nip11-http-oversize")
    return True
