import { spawn } from "node:child_process";
import fs from "node:fs";
import fsp from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import http from "node:http";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

function log(msg) {
  process.stdout.write(`[runner] ${msg}\n`);
}

function logErr(msg) {
  process.stderr.write(`[runner] ${msg}\n`);
}

async function mkdirp(p) {
  await fsp.mkdir(p, { recursive: true });
}

function findOnPath(names) {
  const pathEnv = process.env.PATH || "";
  const parts = pathEnv.split(path.delimiter);
  const exts =
    process.platform === "win32"
      ? (process.env.PATHEXT || ".EXE;.CMD;.BAT;.COM").split(";")
      : [""];

  for (const name of names) {
    for (const dir of parts) {
      for (const ext of exts) {
        const full = path.join(dir, `${name}${ext}`);
        try {
          fs.accessSync(full, fs.constants.X_OK);
          return full;
        } catch {
          // continue
        }
      }
    }
  }
  return null;
}

async function getFreePort() {
  return await new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.on("error", reject);
    srv.listen(0, "127.0.0.1", () => {
      const addr = srv.address();
      const port = typeof addr === "object" && addr ? addr.port : null;
      srv.close(() => resolve(port));
    });
  });
}

async function httpReady(url, timeoutMs) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      await new Promise((resolve, reject) => {
        const req = http.get(url, (res) => {
          res.resume();
          if (res.statusCode && res.statusCode >= 200 && res.statusCode < 500) {
            resolve();
          } else {
            reject(new Error(`HTTP ${res.statusCode}`));
          }
        });
        req.on("error", reject);
        req.setTimeout(2000, () => {
          req.destroy(new Error("timeout"));
        });
      });
      return true;
    } catch {
      await new Promise((r) => setTimeout(r, 200));
    }
  }
  return false;
}

function spawnLogged(cmd, args, opts, outStream, errStream) {
  const p = spawn(cmd, args, {
    ...opts,
    stdio: ["ignore", "pipe", "pipe"]
  });

  if (p.stdout && outStream) p.stdout.pipe(outStream);
  if (p.stderr && errStream) p.stderr.pipe(errStream);

  return p;
}

function killProcessTree(proc) {
  if (!proc || proc.killed) return;
  try {
    if (process.platform !== "win32") {
      // kill the whole process group
      process.kill(-proc.pid, "SIGTERM");
    } else {
      proc.kill("SIGTERM");
    }
  } catch {
    try {
      proc.kill("SIGTERM");
    } catch {
      // ignore
    }
  }
}

async function main() {
  const gnostrBin = process.env.GNOSTR_BINARY;
  if (!gnostrBin) {
    logErr("GNOSTR_BINARY env var is required (path to built gnostr).");
    process.exit(2);
  }

  const broadwayd =
    process.env.GNOSTR_BROADWAYD ||
    findOnPath(["gtk4-broadwayd", "broadwayd"]);
  if (!broadwayd) {
    logErr(
      "Could not find gtk4-broadwayd or broadwayd on PATH. Set GNOSTR_BROADWAYD to the executable."
    );
    process.exit(2);
  }

  const artifactsDir =
    process.env.GNOSTR_E2E_ARTIFACTS_DIR ||
    path.resolve(process.cwd(), "playwright-artifacts");
  await mkdirp(artifactsDir);

  const tmpRoot = await fsp.mkdtemp(path.join(os.tmpdir(), "gnostr-e2e-"));
  const dbDir = path.join(tmpRoot, "ndb");
  const configDir = path.join(tmpRoot, "config");
  const configPath = path.join(configDir, "config.ini");
  await mkdirp(dbDir);
  await mkdirp(configDir);

  // Minimal deterministic relay config for UI
  const configIni =
    "[relays]\n" +
    "urls=wss://relay.damus.io/;wss://nos.lol/;\n";
  await fsp.writeFile(configPath, configIni, "utf8");

  // Fixture path (repo-relative)
  const repoFixtures = path.resolve(
    __dirname,
    "..",
    "..",
    "fixtures",
    "seed.jsonl"
  );
  const seedJsonl = process.env.GNOSTR_E2E_SEED_JSONL || repoFixtures;

  const port = process.env.GNOSTR_E2E_PORT
    ? Number(process.env.GNOSTR_E2E_PORT)
    : await getFreePort();

  // Broadway uses a display number (not a TCP port). We'll pick a random-ish small integer.
  const display = process.env.GNOSTR_BROADWAY_DISPLAY
    ? Number(process.env.GNOSTR_BROADWAY_DISPLAY)
    : (1000 + port) % 100;

  const baseUrl =
    process.env.GNOSTR_BROADWAY_URL || `http://127.0.0.1:${port}`;

  const broadwayOut = fs.createWriteStream(
    path.join(artifactsDir, "broadwayd.log"),
    { flags: "w" }
  );
  const gnostrStdout = fs.createWriteStream(
    path.join(artifactsDir, "gnostr.stdout.log"),
    { flags: "w" }
  );
  const gnostrStderr = fs.createWriteStream(
    path.join(artifactsDir, "gnostr.stderr.log"),
    { flags: "w" }
  );

  log(`Artifacts: ${artifactsDir}`);
  log(`Temp root: ${tmpRoot}`);
  log(`DB dir: ${dbDir}`);
  log(`Config path: ${configPath}`);
  log(`Seed: ${seedJsonl}`);
  log(`Broadwayd: ${broadwayd}`);
  log(`Broadway display: :${display}`);
  log(`Broadway URL: ${baseUrl}`);

  // Start broadwayd
  const broadwayArgs = [`:${display}`, "--port", String(port), "--address", "127.0.0.1"];
  const broadwayProc = spawn(broadwayd, broadwayArgs, {
    cwd: tmpRoot,
    env: process.env,
    detached: process.platform !== "win32"
  });
  if (broadwayProc.stdout) broadwayProc.stdout.pipe(broadwayOut);
  if (broadwayProc.stderr) broadwayProc.stderr.pipe(broadwayOut);

  let gnostrProc = null;

  const cleanup = async (code) => {
    log(`Cleaning up (exit code ${code})...`);
    if (gnostrProc) killProcessTree(gnostrProc);
    killProcessTree(broadwayProc);

    await new Promise((r) => setTimeout(r, 500));

    try {
      broadwayOut.end();
      gnostrStdout.end();
      gnostrStderr.end();
    } catch {
      // ignore
    }

    try {
      await fsp.rm(tmpRoot, { recursive: true, force: true });
    } catch {
      // ignore
    }
  };

  process.on("SIGINT", async () => {
    await cleanup(130);
    process.exit(130);
  });
  process.on("SIGTERM", async () => {
    await cleanup(143);
    process.exit(143);
  });

  broadwayProc.on("exit", (code, signal) => {
    logErr(`broadwayd exited (code=${code}, signal=${signal})`);
  });

  const serverOk = await httpReady(baseUrl, 15_000);
  if (!serverOk) {
    logErr("Broadway server did not become ready in time.");
    await cleanup(1);
    process.exit(1);
  }

  // Start gnostr in Broadway mode
  const gnostrEnv = {
    ...process.env,
    GDK_BACKEND: "broadway",
    BROADWAY_DISPLAY: `:${display}`,
    GNOSTR_LIVE: "FALSE",
    GNOSTR_E2E: "1",
    GNOSTR_DB_DIR: dbDir,
    GNOSTR_CONFIG_PATH: configPath,
    GNOSTR_E2E_SEED_JSONL: seedJsonl,
    GNOSTR_E2E_READY_FILE: process.env.GNOSTR_E2E_READY_FILE || "",
    G_MESSAGES_DEBUG: process.env.G_MESSAGES_DEBUG || "all"
  };

  gnostrProc = spawn(gnostrBin, [], {
    cwd: tmpRoot,
    env: gnostrEnv,
    detached: process.platform !== "win32",
    stdio: ["ignore", "pipe", "pipe"]
  });

  if (gnostrProc.stdout) gnostrProc.stdout.pipe(gnostrStdout);
  if (gnostrProc.stderr) gnostrProc.stderr.pipe(gnostrStderr);

  gnostrProc.on("exit", (code, signal) => {
    logErr(`gnostr exited (code=${code}, signal=${signal})`);
  });

  // Wait for GNOSTR_E2E_READY marker
  const readyTimeoutMs = Number(process.env.GNOSTR_E2E_READY_TIMEOUT_MS || 30_000);
  const readyMarker = "GNOSTR_E2E_READY";
  log(`Waiting for readiness marker '${readyMarker}' (timeout ${readyTimeoutMs}ms)...`);

  const readyOk = await new Promise((resolve) => {
    let buf = "";
    const timer = setTimeout(() => resolve(false), readyTimeoutMs);

    function onData(chunk) {
      buf += chunk.toString("utf8");
      if (buf.includes(readyMarker)) {
        clearTimeout(timer);
        resolve(true);
      }
      if (buf.length > 64 * 1024) buf = buf.slice(-32 * 1024);
    }

    if (gnostrProc.stdout) gnostrProc.stdout.on("data", onData);
    if (gnostrProc.stderr) gnostrProc.stderr.on("data", onData);
  });

  if (!readyOk) {
    logErr("Timed out waiting for GNOSTR_E2E_READY.");
    await cleanup(1);
    process.exit(1);
  }

  log("GNOSTR_E2E_READY received. Running Playwright tests...");

  // Run Playwright
  const npx = process.platform === "win32" ? "npx.cmd" : "npx";
  const pwEnv = {
    ...process.env,
    GNOSTR_BROADWAY_URL: baseUrl,
    GNOSTR_E2E_ARTIFACTS_DIR: artifactsDir
  };

  const pwProc = spawn(npx, ["playwright", "test"], {
    cwd: path.resolve(__dirname, ".."),
    env: pwEnv,
    stdio: "inherit"
  });

  const exitCode = await new Promise((resolve) => {
    pwProc.on("exit", (code) => resolve(code ?? 1));
  });

  await cleanup(exitCode);
  process.exit(exitCode);
}

main().catch(async (err) => {
  logErr(`Fatal error: ${err && err.stack ? err.stack : String(err)}`);
  process.exit(1);
});