import { defineConfig, devices } from "@playwright/test";
import path from "path";

const baseURL = process.env.GNOSTR_BROADWAY_URL || "http://127.0.0.1:8080";
const artifactsDir =
  process.env.GNOSTR_E2E_ARTIFACTS_DIR ||
  path.resolve(process.cwd(), "playwright-artifacts");

export default defineConfig({
  testDir: path.join(__dirname, "tests"),
  timeout: 60_000,
  expect: {
    timeout: 15_000
  },
  outputDir: artifactsDir,
  fullyParallel: false,
  workers: 1,
  reporter: [["list"]],
  use: {
    baseURL,
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
    video: "retain-on-failure"
  },
  projects: [
    {
      name: "chromium",
      use: {
        ...devices["Desktop Chrome"]
      }
    }
  ]
});