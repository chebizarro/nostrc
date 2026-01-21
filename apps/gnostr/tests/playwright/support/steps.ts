import type { Page, TestInfo } from "@playwright/test";

type ConsoleEntry = {
  type: string;
  text: string;
  location?: string;
  time: number;
};

const consoleBuffers = new WeakMap<Page, ConsoleEntry[]>();
const consoleCursors = new WeakMap<Page, number>();

function sanitize(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9-_]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/(^-|-$)/g, "")
    .slice(0, 80);
}

function ensureConsoleCapture(page: Page) {
  if (consoleBuffers.has(page)) return;

  const buf: ConsoleEntry[] = [];
  consoleBuffers.set(page, buf);
  consoleCursors.set(page, 0);

  page.on("console", (msg) => {
    const loc = msg.location();
    const locStr =
      loc && loc.url ? `${loc.url}:${loc.lineNumber}:${loc.columnNumber}` : "";
    buf.push({
      type: msg.type(),
      text: msg.text(),
      location: locStr || undefined,
      time: Date.now()
    });
  });

  page.on("pageerror", (err) => {
    buf.push({
      type: "pageerror",
      text: err && err.stack ? err.stack : String(err),
      time: Date.now()
    });
  });
}

async function attachConsoleSince(page: Page, testInfo: TestInfo, name: string) {
  ensureConsoleCapture(page);

  const buf = consoleBuffers.get(page) || [];
  const cursor = consoleCursors.get(page) || 0;
  const slice = buf.slice(cursor);
  consoleCursors.set(page, buf.length);

  const payload = slice
    .map((e) => {
      const ts = new Date(e.time).toISOString();
      const loc = e.location ? ` (${e.location})` : "";
      return `${ts} [${e.type}]${loc} ${e.text}`;
    })
    .join("\n");

  await testInfo.attach(`console-${name}`, {
    body: payload || "(no new console output)",
    contentType: "text/plain"
  });
}

export async function stepWithCapture(
  page: Page,
  testInfo: TestInfo,
  name: string,
  fn: () => Promise<void>
): Promise<void> {
  const safe = sanitize(name);

  await testInfo.step(name, async () => {
    await fn();

    // DOM snapshot
    const html = await page.content();
    await testInfo.attach(`dom-${safe}`, {
      body: html,
      contentType: "text/html"
    });

    // Screenshot (Broadway renders into web canvas; screenshot is still useful)
    const png = await page.screenshot({ fullPage: true });
    await testInfo.attach(`screenshot-${safe}`, {
      body: png,
      contentType: "image/png"
    });

    // Incremental console output
    await attachConsoleSince(page, testInfo, safe);
  });
}