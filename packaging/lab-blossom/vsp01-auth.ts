// nginx auth_request front door. The upstream store is never host-published.
import { parseAuthEvent } from "./src/middleware/auth.ts";

const lines = (await Deno.readTextFile("/config/allowlist.txt"))
  .split(/\r?\n/).map((s) => s.trim()).filter((s) => s && !s.startsWith("#"));
if (lines.length > 256 || lines.some((s) => !/^[0-9a-f]{64}$/.test(s))) {
  throw new Error("allowlist must contain at most 256 lowercase hex pubkeys");
}
const allowlist = new Set(lines); // Empty means deny all. Restart to reload.
const denied = () => new Response(null, {
  status: 401,
  headers: { "WWW-Authenticate": "Nostr", "Cache-Control": "no-store" },
});

Deno.serve({ hostname: "0.0.0.0", port: 3001 }, (req) => {
  try {
    const header = req.headers.get("authorization");
    if (!header?.startsWith("Nostr ")) return denied();
    const event = parseAuthEvent(header.slice(6), req.headers.get("x-original-host"));
    if (!allowlist.has(event.pubkey)) return denied();
    const method = req.headers.get("x-original-method");
    const path = new URL(req.headers.get("x-original-uri") ?? "/", "http://lab").pathname;
    const verb = event.tags.find((t) => t[0] === "t")?.[1];
    let expected: string;
    let hash: string | null = null;
    let hashRequired = false;
    if (path === "/upload" && (method === "PUT" || method === "HEAD")) {
      expected = "upload";
      hash = req.headers.get("x-sha-256");
      hashRequired = true;
    } else if (path === "/mirror" && method === "PUT") {
      expected = "upload";
      // The upstream mirror handler validates the body hash against x tags.
      if (!event.tags.some((t) => t[0] === "x" && /^[0-9a-f]{64}$/.test(t[1]))) return denied();
    } else if (/^\/list\/[0-9a-f]{64}$/.test(path) && method === "GET") {
      expected = "list";
    } else {
      const match = path.match(/^\/([0-9a-f]{64})(?:\.[a-zA-Z0-9]+)?$/);
      if (!match || !["GET", "HEAD", "DELETE"].includes(method ?? "")) return denied();
      hash = match[1];
      expected = method === "DELETE" ? "delete" : "get";
      hashRequired = method === "DELETE";
    }
    if (verb !== expected) return denied();
    const hashes = event.tags.filter((t) => t[0] === "x").map((t) => t[1]);
    if ((hashRequired || hashes.length > 0) && path !== "/mirror" &&
        (!hash || !hashes.includes(hash))) return denied();
    return new Response(null, { status: 204 });
  } catch {
    // Invalid auth has the same response as a nonmember, before any lookup.
    return denied();
  }
});
