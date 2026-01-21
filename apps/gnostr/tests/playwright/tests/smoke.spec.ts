import { expect, test } from "@playwright/test";
import { stepWithCapture } from "../support/steps";

test.describe("GNostr Broadway smoke", () => {
  test("timeline shows seeded note content", async ({ page }, testInfo) => {
    await stepWithCapture(page, testInfo, "open app", async () => {
      await page.goto("/", { waitUntil: "domcontentloaded" });
    });

    await stepWithCapture(page, testInfo, "timeline list visible", async () => {
      // Prefer semantic targeting via GTK accessibility labels.
      const timeline = page.getByLabel("Timeline List");
      await expect(timeline).toBeVisible();
    });

    await stepWithCapture(page, testInfo, "seeded root note visible", async () => {
      await expect(page.getByText("E2E: Root note")).toBeVisible();
    });
  });

  test("clicking Note Reply switches composer to reply mode", async ({ page }, testInfo) => {
    await stepWithCapture(page, testInfo, "open app", async () => {
      await page.goto("/", { waitUntil: "domcontentloaded" });
    });

    await stepWithCapture(page, testInfo, "click first Note Reply", async () => {
      await page.getByRole("button", { name: "Note Reply" }).first().click();
    });

    await stepWithCapture(page, testInfo, "reply indicator visible", async () => {
      await expect(page.getByText(/Replying to/i)).toBeVisible();
    });

    await stepWithCapture(page, testInfo, "composer post button becomes Reply", async () => {
      const post = page.getByRole("button", { name: "Composer Post" });
      await expect(post).toBeVisible();
      await expect(post).toHaveText(/Reply/i);
    });
  });

  test("relay manager dialog opens and shows relay list", async ({ page }, testInfo) => {
    await stepWithCapture(page, testInfo, "open app", async () => {
      await page.goto("/", { waitUntil: "domcontentloaded" });
    });

    await stepWithCapture(page, testInfo, "open relays dialog", async () => {
      await page.getByRole("button", { name: "Manage Relays" }).click();
    });

    await stepWithCapture(page, testInfo, "relay entry and list visible", async () => {
      await expect(page.getByPlaceholder("wss://example.com")).toBeVisible();
      await expect(page.getByText(/Tips: Press/i)).toBeVisible();
    });
  });
});