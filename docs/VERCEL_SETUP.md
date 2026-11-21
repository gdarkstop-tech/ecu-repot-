# Put the robot's AI gateway on Vercel (free Hobby plan)

The gateway is the small online service the robot talks to. It holds your Groq key, turns the
child's voice into text (Groq Whisper, Arabic + English), asks the language model (Groq
`openai/gpt-oss-120b`), checks the answer for child safety, and sends back text for the OLED —
and, if the robot has a speaker, a natural voice (Groq Orpheus). On Vercel it runs online all the
time, so your laptop can be switched off.

```
robot (Wi-Fi) ──HTTPS──▶ https://<your-project>.vercel.app ──▶ Groq Whisper   (speech → text)
                                                           ├─▶ Groq gpt-oss-120b (the answer)
                                                           └─▶ Groq Orpheus   (answer → voice, optional)
```

**Costs.** Vercel Hobby: $0, no credit card (personal, non-commercial use). Groq has a free plan
with daily limits (see "How it fits the free plan" below); the gateway never upgrades a plan or
switches to a paid service by itself. Claude (Anthropic) is still available as an option — it
needs prepaid Anthropic credits ([CLAUDE_SETUP.md](CLAUDE_SETUP.md)).
**Already deployed? Go straight to [Part G](#part-g--switch-an-existing-deployment-to-groq).**

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

You also need your **Groq API key** (console.groq.com → API Keys, starts with `gsk_`). One key
covers speech recognition, the language model and the voice. On Groq's free plan there is no
bill: when a limit is reached, requests are refused until it resets. (Only if you choose Claude:
an Anthropic API key with prepaid credits and a monthly spend limit.)

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
   | `LLM_PROVIDER` | `groq` |
   | `GROQ_LLM_MODEL` | `openai/gpt-oss-120b` |
   | `STT_PROVIDER` | `groq` |
   | `GROQ_API_KEY` | your Groq key (`gsk_...`) |
   | `TTS_PROVIDER` | `groq` if the robot has a speaker (natural voice), otherwise `mock` |
   | `DEVICE_TOKEN_HASHES` | value 2 (starts with `robot-1:`) |
   | `CONTEXT_SECRET` | value 3 (the random text) |
   | `ROBOT_NAME` | your robot's name, for example `Robo` (optional) |

   Type the keys exactly as shown (capital letters, underscores). Do not add quotes around values.
   If Vercel shows a **Sensitive** switch, turn it on for `GROQ_API_KEY` and `CONTEXT_SECRET`.
   Optional voices: `GROQ_TTS_VOICE_EN` (default `hannah`; also autumn, diana, austin, daniel,
   troy) and `GROQ_TTS_VOICE_AR` (default `lulwa`; also abdullah, fahad, sultan, noura, aisha).

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
   {"ok":true,"version":"0.4.0","stt":"groq","llm":"groq","tts":"groq",
    "llm_model":"openai/gpt-oss-120b",
    "tts_model":"canopylabs/orpheus-v1-english / canopylabs/orpheus-arabic-saudi",
    "devices":1,"stateless":true,"memory":"context","context_key":"CONTEXT_SECRET",
    "warnings":[],"errors":[]}
   ```

   Check: `"ok":true`, `"version":"0.4.0"`, `"llm":"groq"`, `"llm_model":"openai/gpt-oss-120b"`,
   `"devices":1`, `"context_key":"CONTEXT_SECRET"`, and `"warnings":[]` (`"tts":"mock"` and
   `"tts_model":null` if you chose no voice). No key or secret is ever shown here.

4. The same check from PowerShell (in the `gateway` folder):

   ```powershell
   ..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --health
   ```

**If `/healthz` is not right:**

| You see | Meaning | Fix |
|---|---|---|
| Vercel `404: NOT_FOUND` page | Root Directory is not `gateway`, or the preset is not FastAPI | Project **Settings**: set Root Directory to `gateway` and Framework Preset to FastAPI, save, redeploy |
| A Vercel login page | You opened a deployment address, not the production domain | Use the short `….vercel.app` domain from **Domains** |
| `"ok":false` and `"errors":["stt: ..."]` / `["llm: ..."]` / `["tts: ..."]` | `GROQ_API_KEY` missing or misspelled, or a provider name typo | Add/fix it, then redeploy (Part F) |
| `"version"` older than `0.4.0` | Vercel has not deployed the latest code yet | Deployments → wait for the newest one, or Redeploy |
| `"llm":"anthropic"` | `LLM_PROVIDER` still says `anthropic` | Change it to `groq` (Part G), redeploy |
| `"warnings":["ANTHROPIC_API_KEY is not set"]` | You chose Claude but have no Anthropic key | Use `LLM_PROVIDER=groq`, or add the key |
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
start a new topic. Then an **open question** — only the language model can answer it:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --no-speaker --text "Why do cats purr?"
```

`"source":"llm"` means Groq gpt-oss-120b answered. (`"source":"math"`, `"tutor"` or `"safety"`
come from the built-in tutor and safety rules; `"source":"fallback"` with a warning such as
`llm_rate_limited` or `llm_auth_failed` means Groq refused — see the table in Part G.) Arabic:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --no-speaker --text "لماذا السماء زرقاء؟"
```

**The voice** (needs `TTS_PROVIDER=groq`): run the same command **without** `--no-speaker`:

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://robot-gateway.vercel.app --token rbt_... --text "Why is the sky blue?"
```

The reply shows `"audio": {..., "sample_rate": 24000}` and the tool saves **`reply.wav`** —
open it on the laptop: that is exactly the 24 kHz audio the robot will play. Each voice answer
uses 1–2 of Groq's ~100 daily speech requests, so test it a few times, not hundreds.

---

## Part E — Point the robot at Vercel

1. Flash firmware **0.4.0** or newer (it carries the conversation memory between questions and,
   with a speaker, shows the answer as text whenever the voice is unavailable):

   ```powershell
   cd C:\Users\gdark\OneDrive\Documents\robot\firmware
   ```

   ```powershell
   pio run -e robot -t upload
   ```

   Firmware 0.3.0 also works with this gateway (it plays the Groq voice too), but when the voice
   is unavailable it shows only a one-line subtitle. Firmware 0.2.0 works but forgets everything
   between questions.

2. Provision the robot (USB, once):

   ```powershell
   cd ..
   ```

   ```powershell
   .venv\Scripts\python.exe tools\robot_console.py provision
   ```

   Enter your Wi-Fi (2.4 GHz), the gateway URL **`https://robot-gateway.vercel.app`** (no `/` or
   `/healthz` at the end), the `rbt_...` token (value 1), language and age. "Speaker installed?":
   **Yes** if the MAX98357A and speaker are wired (and `TTS_PROVIDER=groq`), otherwise **No**.

   Loudness: the robot's `volume` setting is squared into a gain, so `set volume 70` gives the
   0.50 gain you tested with your 8 Ω speaker (70 × 70 / 10 000 ≈ 0.49); the default 60 is a bit
   quieter (0.36). The gateway also limits every answer to −1 dBFS, so it never sends clipped
   audio. In the console: `set volume 70` (applies immediately).

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

## Part G — Switch an existing deployment to Groq

For a project that was set up with Claude (`LLM_PROVIDER=anthropic`) — for example after
Anthropic reports `Your credit balance is too low`. Your existing secrets stay as they are.

1. Vercel → your project → **Settings** → **Environment Variables**.
2. Change and add exactly these (click **⋯ → Edit** on an existing row):

   | Key | Action | Value |
   |---|---|---|
   | `LLM_PROVIDER` | **edit** (was `anthropic`) | `groq` |
   | `GROQ_LLM_MODEL` | **add** | `openai/gpt-oss-120b` |
   | `TTS_PROVIDER` | **edit** (was `mock`) — only if the robot has its speaker | `groq` |
   | `GROQ_TTS_VOICE_EN` | add (optional) | `hannah` |
   | `GROQ_TTS_VOICE_AR` | add (optional) | `lulwa` |

   Do **not** change `GROQ_API_KEY`, `CONTEXT_SECRET`, `DEVICE_TOKEN_HASHES` or `STT_PROVIDER`.
   `ANTHROPIC_API_KEY` and `ANTHROPIC_MODEL` can stay; they are simply not used while
   `LLM_PROVIDER` is `groq` (switch back to `anthropic` any time after adding credits).
3. **Deployments** → newest deployment → **⋯** → **Redeploy** (variables only apply to new
   deployments).
4. Open `https://robot-gateway.vercel.app/healthz` — check `"version":"0.4.0"`, `"llm":"groq"`,
   `"llm_model":"openai/gpt-oss-120b"` and (with a voice) `"tts":"groq"`.
5. Run the Part D commands (open question, then the voice test that saves `reply.wav`).
6. Robot: flash firmware 0.4.0 (Part E step 1); if the speaker is wired, in `robot_console.py`
   type `set speaker on`, then `reboot`, then `set volume 70`. Ask "Why do cats purr?".

**What the logs say** (Vercel → project → **Logs**). The gateway logs provider, model, HTTP status
and Groq's error type/code — never your key and never the child's words:

| Log / reply shows | Meaning | What happens on the robot | Fix |
|---|---|---|---|
| `code=rate_limit_exceeded -> llm_rate_limited` | Groq's free per-minute/day limit for the model | Says "So many questions! My brain needs a one-minute break…" | Wait (per-minute) or until tomorrow (per-day) |
| `-> tts_rate_limited` (audio request answers 429) | Free daily voice quota (~100 requests) used up | Shows the whole answer as scrolling text instead (firmware 0.4.0) | Wait until tomorrow, or set `TTS_PROVIDER=mock` |
| `-> stt_rate_limited` (turn answers 429) | Whisper limit reached | Short "busy" error face, then listens again | Wait a minute |
| `code=invalid_api_key -> llm_auth_failed` / `tts_auth_failed` | `GROQ_API_KEY` wrong or revoked | Friendly "rest" answer / text instead of voice | Fix the key in Vercel, redeploy |
| `-> tts_terms_required` | Groq wants the Orpheus terms accepted in its console | Text instead of voice | Open the model in console.groq.com Playground once and accept |
| `-> llm_model_not_found` | `GROQ_LLM_MODEL` misspelled or retired | Friendly "rest" answer | Fix the model name, redeploy |

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
| Request and response size: max 4.5 MB | Longest question: 15 s of 16 kHz audio = 480 KB. Text answers are a few KB. Speech audio is capped at 4 MB (a two-piece answer is about 0.5 MB). |
| Groq free plan (limits shown on Groq's rate-limits page; your own are at console.groq.com → Settings → Limits) | gpt-oss-120b: 30 requests/min, 1,000/day, 8,000 tokens/min. Whisper: 20/min, 2,000/day. **Orpheus voice: 10/min, 100/day** and at most 200 characters per request. The gateway speaks at most 2 pieces of ≤ 200 characters per answer (`GROQ_TTS_MAX_CHUNKS`), asks the model for answers under 40 words, retries only when Groq asks for a pause of ≤ 2 s, and otherwise answers kindly or falls back to text. It never upgrades your plan or calls a paid service. |
| Time limit: 300 s per request | AI calls give up after 25 s; the robot waits at most 25 s and then shows an error face and recovers. |
| Monthly allowance: 1,000,000 function calls, 4 h of active CPU, 360 GB-h of memory | One question = one call (two with a speaker). Most of the time is spent waiting for Groq/Claude. If the allowance ran out, Vercel pauses the project until the next month instead of charging you. |
| Several instances at once | The gateway's own limits (20 questions/min, 1,500/day per robot) are counted per instance, so they are approximate here. Groq's plan limits are the hard cap. |
| No `espeak-ng` on Vercel | Voice comes from Groq Orpheus (`TTS_PROVIDER=groq`), converted to 24 kHz 16-bit mono PCM and streamed piece by piece, so the robot starts talking after the first piece. Without a speaker (`X-Robot-Output: text`) no voice is requested at all. |
| First request after a quiet period is slower ("cold start") | Not measured on Vercel yet. The gateway's own start-up (importing the code) took 0.3 s in testing. |

Vercel facts above come from Vercel's documentation: [Functions limits](https://vercel.com/docs/functions/limitations),
[Python runtime](https://vercel.com/docs/functions/runtimes/python),
[FastAPI on Vercel](https://vercel.com/docs/frameworks/backend/fastapi),
[Hobby plan](https://vercel.com/docs/plans/hobby),
[Deployment protection](https://vercel.com/docs/deployment-protection),
[Production branch](https://vercel.com/kb/guide/can-i-use-a-non-default-branch-for-production).
Limits can change — check those pages if something looks different.

Groq facts come from Groq's documentation: [Text to speech](https://console.groq.com/docs/text-to-speech),
[Orpheus](https://console.groq.com/docs/text-to-speech/orpheus), [Rate limits](https://console.groq.com/docs/rate-limits),
[Reasoning](https://console.groq.com/docs/reasoning), [GPT-OSS 120B](https://console.groq.com/docs/model/openai/gpt-oss-120b).

Technical details: [ARCHITECTURE.md](ARCHITECTURE.md) (ADR-11, ADR-12) · [PROTOCOL.md](PROTOCOL.md).
