# AuDuo — Project Knowledge & Ground Truth

> **CRITICAL DIRECTIVE FOR ALL AGENTS & CONCURRENT SESSIONS:**  
> Read this document before making any changes to domain routing, payments, release packaging, or deployment configurations. Do NOT deviate from these guidelines based on assumption.

---

## 1. Domain & Web Hosting Architecture

- **Canonical Production Site:** [`https://getauduo.vercel.app`](https://getauduo.vercel.app)
- **Website Repository:** `d:\projects\auduo-web` (`https://github.com/AivinKoshy/auduo-web`, branch `main`).
- **Legacy Redirects:**
  - `d:\projects\AuDuo\docs\donate\index.html` (served via GitHub Pages at `https://aivinkoshy.github.io/auduo/donate/`) contains a 0-second `<meta http-equiv="refresh">` redirect forwarding all traffic to `https://getauduo.vercel.app/#support`.
- **STRICT DOMAIN RULE (DO NOT SWITCH TO CLOUDFLARE PAGES / `.pages.dev`):**
  - The payment processor (Dodo Payments) strictly reviewed and verified the merchant account against the live domain `getauduo.vercel.app` (including active Privacy Policy at `/privacy.html` and Refund Policy at `/refund.html`).
  - **DO NOT** attempt to change domains or migrate to `.pages.dev`. Changing domains immediately after merchant verification triggers compliance re-audits, fraud flags, or payout holds.

---

## 2. Payment Architecture & Merchant Configuration

- **Merchant Provider:** [Dodo Payments](https://dodopayments.com) (Merchant of Record / MoR).
- **Merchant Account:** `Aivin Koshy` (Individual).
- **Live Supporter Pass Product:**
  - **Product ID:** `pdt_0NpIU9hS5317dggr2L4UP`
  - **Product Name:** `AuDuo Supporter Pass`
  - **Pricing Model:** Pay What You Want (One-time donation / supporter pass).
    - Minimum: \$2.00 USD
    - Suggested / Default: \$5.00 USD
  - **Tax Mode:** Tax Exclusive (taxes such as GST/VAT are added on top at checkout rather than eating into proceeds, maintaining transparency).
  - **Checkout URL:** `https://checkout.dodopayments.com/buy/pdt_0NpIU9hS5317dggr2L4UP?quantity=1`
- **Direct UPI (Domestic India):**
  - UPI ID: `aivin@upi`
  - Rendered on the website alongside the QR code for zero-fee domestic transfers.

---

## 3. Releases & Distribution Guidelines

- **Primary Binary:** Windows Installer (`AuDuo_Setup_v*.exe`) generated with Inno Setup (`installer.iss`).
- **Portable Binary Policy:**
  - **Removed / Discontinued:** Portable zero-install binaries (`auduo.exe`) have been removed from GitHub releases.
  - **Rationale:** Ensures all general users install via the setup wizard, avoiding issues with missing virtual audio drivers, desktop shortcut registration, or corrupted loose files.
- **Branch Ground Truth & Working Rules:**
  - **`main` is the SOLE canonical branch** for all architecture, audio engine, and core utility logic (including the newly merged N-device audio engine and multi-channel sync).
  - **`feat/skeuomorphic-gui` is STRICTLY IGNORED** for all AGY intents and purposes. Do not read, audit, or base any engine/system decisions on `feat/skeuomorphic-gui`.
- **Current Release Status:**

---

## 4. Deployment Protocols & Gotchas

- **Vercel Deployments (`d:\projects\auduo-web`):**
  - **Commit Author Requirement:** Any git commit pushed or deployed via Vercel MUST use `aivinkoshy456@gmail.com`. Commits authored with a different email will trigger Vercel deployment authorization errors.
  - **Deployment Command:** Run `vercel deploy --prod --yes` followed by `vercel alias set <deployment-url> getauduo.vercel.app`.
  - Static HTML files reside directly in repository root (`index.html`, `privacy.html`, `refund.html`). Do not use custom rewrites in `vercel.json` that break root asset resolution.
- **Git Push Gate:**
  - Local builds, tests, and commits are encouraged.
  - **NEVER execute `git push`** to remote repositories without explicit user permission.

---

## 5. Application Launching & Interactive Desktop Protocol (MANDATORY)

- **Always Launch Interactive GUI Applications via `windows-mcp`:**
  - Standard command execution (`run_command`) operates inside an isolated virtual session (`WinSta0\exebox-...`) where windows and graphical interfaces are headless, hidden, or invisible to the user.
  - Whenever launching `auduo.exe` or any application/utility that the **USER needs to see and interact with**, **ALWAYS route the launch through the `windows-mcp` MCP server** (e.g., using `windows-mcp` `PowerShell` or `App` tool).
  - This guarantees the application window is rendered directly on the active, interactive user desktop (`WinSta0\Default`).

---

## 6. External LLM Codebase Audit & Claude Integration (Abhibots Gateway)

- **Endpoint & Credentials:**
  - **Base URL:** `https://opus.abhibots.com/v1`
  - **Protocol:** Standard OpenAI-compatible Chat Completions (`/v1/chat/completions` or `/v1/models`).
  - **Auth Header:** `Authorization: Bearer ak-1Q9HJWMCjr2PAILDk87IoPKKlByRf1Id`
- **Supported Models & Operating Windows:**
  - **`claude-opus-5-5`**: Primary model for deep architectural audits, real-time concurrency, and lock-free audio debugging. Available around the clock.
  - **`claude-fable-5-1`**: Secondary model for code reviews and high-level opinions. Operates on off-peak hours (free plan opens 1:00 AM – 6:00 AM IST / 11:00 PM IST; returns HTTP 429 during daytime peak hours).
- **Execution Protocol:**
  - Target files strictly from canonical `main` (`include/AudioEngine.hpp`, `include/RingBuffer.hpp`, `src/AudioEngine.cpp`). Never feed `feat/skeuomorphic-gui`.
  - Assemble payloads under ~80k characters (~20k tokens) to prevent gateway timeouts or HTTP 502 errors.
  - Reusable helper scripts reside in `tools/` (e.g. `audit_main_core.py`, `audit_fable_focused.py`).
