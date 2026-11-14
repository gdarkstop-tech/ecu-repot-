# Put the robot's AI gateway on Vercel (free Hobby plan)

The gateway is the small online service the robot talks to. It holds your Claude and Groq keys,
turns the child's voice into text (Groq Whisper, Arabic + English), asks Claude, checks the answer
for child safety, and sends back text for the OLED. On Vercel it runs online all the time, so your
laptop can be switched off.

```
robot (Wi-Fi) ──HTTPS──▶ https://<your-project>.vercel.app ──▶ Groq Whisper (speech → text)
                                                           └─▶ Claude Haiku 5.5 (the answer)
```

**Costs.** Vercel Hobby: $0, no credit card (personal, non-commercial use). You still pay
Anthropic and Groq for each question — about $0.0004 per question (estimate, see
[CLAUDE_SETUP.md](CLAUDE_SETUP.md#costs-estimates--confirm-on-each-providers-pricing-page)).
Nothing else in this guide costs money.

**Keys stay secret.** Your API keys go only into Vercel's *Environment Variables* screen — never
into GitHub, never into the robot. The robot only has its own device token, which you can cancel
at any time.

---

## Part A — Prepare three values on your laptop (5 minutes)

1. Open **PowerShell** in your project folder:

   ```powershell
   cd C:\Users\gdark\OneDrive\Documents\robot
   ```

2. Get the latest code (if you downloaded a ZIP instead of using git, download it again):

   ```powershell
   git pull origin claude/vigilant-gauss-lvs43y
   ```

3. Create the robot's device token:

   ```powershell
   cd gateway
   ```

   ```powershell
   ..\.venv\Scripts\python.exe -m robot_gateway.devices add robot-1 --file devices.json
   ```

   It prints two lines you need:

   - `Token (shown ONCE ...): rbt_...` → **value 1**, goes into the robot (Part E).
   - `... DEVICE_TOKEN_HASHES=robot-1:...` → **value 2**, the part *after* `DEVICE_TOKEN_HASHES=`
     (it starts with `robot-1:`), goes into Vercel.

   If it says `device 'robot-1' already exists` and you no longer have that token, run the same
   command with a new name, for example `robot-2`.

4. Create a random secret for the robot's conversation memory:

   ```powershell
   ..\.venv\Scripts\python.exe -c "import secrets; print(secrets.token_urlsafe(48))"
   ```

   The long random text it prints is **value 3** (`CONTEXT_SECRET`).

5. Keep the three values in Notepad **without saving the file** until Part B and Part E are done.
   Do not save keys inside the project folder: it is synced by OneDrive and is a git repository.

You also need your **Anthropic API key** (console.anthropic.com → API Keys, starts with
`sk-ant-`) and your **Groq API key** (console.groq.com → API Keys, starts with `gsk_`). In the
Anthropic Console, set a **monthly spend limit** (Settings → Limits) — that is your real cost cap.

---

## Part B — Import the project into Vercel (exact settings)

1. Go to **vercel.com** and log in.
2. Click **Add New…** → **Project**.
3. Under **Import Git Repository**, find **`ecu-repot-`** and click **Import**.
4. On the **New Project** screen, set exactly:

   | Setting | Value |
   |---|---|
   | Vercel Team | your Hobby account (already selected) |
   | Project Name | `robot-gateway` (lowercase; it becomes part of your web address) |
   | **Root Directory** | click **Edit** → choose the **`gateway`** folder → **Continue** |
   | **Framework Preset** | **FastAPI** (it should switch to FastAPI after you choose `gateway`; if not, pick FastAPI) |
   | Build and Output Settings | leave everything as it is — no overrides |

5. Open **Environment Variables** on the same screen and add these, one row each
   (**Key** on the left, **Value** on the right, then **Add**):

   | Key | Value |
   |---|---|
   | `LLM_PROVIDER` | `anthropic` |
   | `ANTHROPIC_API_KEY` | your Anthropic key (`sk-ant-...`) |
   | `ANTHROPIC_MODEL` | `claude-haiku-5-5` |
   | `STT_PROVIDER` | `groq` |
   | `GROQ_API_KEY` | your Groq key (`gsk_...`) |
   | `TTS_PROVIDER` | `mock` |
   | `DEVICE_TOKEN_HASHES` | value 2 (starts with `robot-1:`) |
   | `CONTEXT_SECRET` | value 3 (the random text) |
   | `ROBOT_NAME` | your robot's name, for example `Robo` (optional) |

   Type the keys exactly as shown (capital letters, underscores). Do not add quotes around values.
   If Vercel shows a **Sensitive** switch, turn it on for `ANTHROPIC_API_KEY`, `GROQ_API_KEY` and
   `CONTEXT_SECRET`.

6. Click **Deploy** and wait until it says the deployment is ready (usually 1–2 minutes).

7. Check the branch: open the project → **Settings** → **Environments** → **Production** →
   **Branch Tracking**. It should show **`claude/vigilant-gauss-lvs43y`** (it is the repository's
   only and default branch, so Vercel picks it). If it shows another branch, type
   `claude/vigilant-gauss-lvs43y`, save, and redeploy (Part F).

---

## Part C — Test `/healthz` (is the gateway alive and configured?)

1. On the project page, find your **production domain** under **Domains**, for example
   `robot-gateway.vercel.app` (Vercel adds a few letters if the name is taken). Use this short
   address — not the long one with random letters that belongs to a single deployment (those are
   protected by a Vercel login, and the robot cannot log in).

2. Open this in your browser (with your domain):

   ```
   https://robot-gateway.vercel.app/healthz
   ```

3. You should see (on one line):

   ```json
   {"ok":true,"version":"0.3.0","stt":"groq","llm":"anthropic","tts":"mock","devices":1,
    "stateless":true,"memory":"context","context_key":"CONTEXT_SECRET","warnings":[],"errors":[]}
   ```

   Check: `"ok":true`, `"stt":"groq"`, `"llm":"anthropic"`, `"devices":1`,
   `"context_key":"CONTEXT_SECRET"`, and `"warnings":[]`. No key or secret is ever shown here.

4. The same check from PowerShell (in the `gateway` folder):

   ```powershell
   ..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --health
   ```

**If `/healthz` is not right:**

| You see | Meaning | Fix |
|---|---|---|
| Vercel `404: NOT_FOUND` page | Root Directory is not `gateway`, or the preset is not FastAPI | Project **Settings**: set Root Directory to `gateway` and Framework Preset to FastAPI, save, redeploy |
| A Vercel login page | You opened a deployment address, not the production domain | Use the short `….vercel.app` domain from **Domains** |
| `"ok":false` and `"errors":["stt: ..."]` | `GROQ_API_KEY` missing or misspelled | Add/fix it, then redeploy (Part F) |
| `"warnings":["ANTHROPIC_API_KEY is not set"]` | Claude key missing | Add it, then redeploy |
| `"devices":0` | `DEVICE_TOKEN_HASHES` missing or mistyped | Paste value 2 again (it must start with `robot-1:`), redeploy |
| `"context_key":"derived"` | `CONTEXT_SECRET` missing or shorter than 32 characters | Paste value 3, redeploy (works meanwhile, but set it) |
| `500` / `FUNCTION_INVOCATION_FAILED` | The code crashed while starting | Project → **Logs** → read the red lines |

---

## Part D — Test a real conversation from the laptop (before using the robot)

In PowerShell, in the `gateway` folder, replace the address and `rbt_...` with yours:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --no-speaker --text "What is seven plus five?"
```

A reply with `"reply_text"` containing 12 means the robot's path works. Now check that the
gateway **remembers** the conversation even though Vercel keeps nothing between requests:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --no-speaker --text "let's do math" --text "5"
```

Turn 2 must react to the question from turn 1 ("Correct!" or "Not quite. Try again!"), not
start a new topic. Then try a question for Claude in Arabic:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --no-speaker --text "لماذا السماء زرقاء؟"
```

`"source":"llm"` in the reply means Claude answered.

---

## Part E — Point the robot at Vercel

1. Flash firmware **0.3.0** or newer (it carries the conversation memory between questions):

   ```powershell
   cd C:\Users\gdark\OneDrive\Documents\robot\firmware
   ```

   ```powershell
   pio run -e robot -t upload
   ```

   Older firmware (0.2.0) still works with this gateway, but forgets everything between
   questions (a quiz or math game would not continue).

2. Provision the robot (USB, once):

   ```powershell
   cd ..
   ```

   ```powershell
   .venv\Scripts\python.exe tools\robot_console.py provision
   ```

   Enter your Wi-Fi (2.4 GHz), the gateway URL **`https://robot-gateway.vercel.app`** (no `/` or
   `/healthz` at the end), the `rbt_...` token (value 1), language and age, and answer **No** to
   "speaker installed".

3. Check it:

   ```powershell
   .venv\Scripts\python.exe tools\robot_console.py
   ```

   Type `status`: look for `"wifi":{"connected":true` and `"gateway":{"configured":true`. Type
   `ask let's do math`, then answer it out loud after unmuting (short press on the button). After
   the first answer, `status` shows `"memory":true`.

4. Unplug the USB cable, power the robot from the power bank, unmute, and ask a question. The robot
   now needs only Wi-Fi and Vercel.

---

## Part F — Changing settings later

- **Environment variables only apply to new deployments.** After adding or changing one: project
  → **Deployments** → the newest one → **⋯** → **Redeploy**.
- **New code** pushed to `claude/vigilant-gauss-lvs43y` deploys automatically.
- **Lost robot:** delete its entry from `DEVICE_TOKEN_HASHES` (entries are separated by commas),
  redeploy. **Leaked API key:** revoke it in the Anthropic/Groq console, put the new one in Vercel,
  redeploy.
- Changing `CONTEXT_SECRET` only makes every robot start a fresh conversation. Nothing breaks.

---

## How it fits the free plan (and what was changed for it)

| Free-plan rule | How the gateway handles it |
|---|---|
| Nothing is kept between requests (no server memory, no files) | The conversation (learning mode, the pending question, quiz score, the last 4 exchanges) is sealed — encrypted and signed with `CONTEXT_SECRET` — and returned as `ctx` with each answer. The robot keeps it in RAM and sends it back with the next question (`X-Robot-Context`). Any Vercel instance can continue the conversation. A forged, edited, expired (30 min) or other robot's token is ignored and a fresh conversation starts. Rebooting the robot also starts fresh. |
| Request and response size: max 4.5 MB | Longest question: 15 s of 16 kHz audio = 480 KB. Text answers are a few KB. Speech audio (if you add a speaker) is capped at 4 MB. |
| Time limit: 300 s per request | AI calls give up after 25 s; the robot waits at most 25 s and then shows an error face and recovers. |
| Monthly allowance: 1,000,000 function calls, 4 h of active CPU, 360 GB-h of memory | One question = one call (two with a speaker). Most of the time is spent waiting for Groq/Claude. If the allowance ran out, Vercel pauses the project until the next month instead of charging you. |
| Several instances at once | Rate limits (20 questions/min, 1,500/day per robot) are counted per instance, so they are approximate here. Your Anthropic monthly spend limit is the hard cap. |
| No `espeak-ng` on Vercel | `TTS_PROVIDER=mock`. With no speaker the robot never asks for audio. A real voice later needs a speech API (for example OpenAI TTS, paid). |
| First request after a quiet period is slower ("cold start") | Not measured on Vercel yet. The gateway's own start-up (importing the code) took 0.3 s in testing. |

Vercel facts above come from Vercel's documentation: [Functions limits](https://vercel.com/docs/functions/limitations),
[Python runtime](https://vercel.com/docs/functions/runtimes/python),
[FastAPI on Vercel](https://vercel.com/docs/frameworks/backend/fastapi),
[Hobby plan](https://vercel.com/docs/plans/hobby),
[Deployment protection](https://vercel.com/docs/deployment-protection),
[Production branch](https://vercel.com/kb/guide/can-i-use-a-non-default-branch-for-production).
Limits can change — check those pages if something looks different.

Technical details: [ARCHITECTURE.md](ARCHITECTURE.md) (ADR-11) · [PROTOCOL.md](PROTOCOL.md).
