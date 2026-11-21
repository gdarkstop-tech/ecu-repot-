# Talking to Claude — setup guide (microphone + Wi-Fi, answers on the OLED)

> **Default since gateway 0.4.0: Groq (free plan), not Claude.** With `LLM_PROVIDER=groq` the
> answers come from Groq `openai/gpt-oss-120b` using the same `GROQ_API_KEY` as speech
> recognition — see [VERCEL_SETUP.md](VERCEL_SETUP.md) (Part G switches an existing deployment).
> Claude remains a supported option: it needs an Anthropic API key **with prepaid credits**
> (an empty balance shows up in the gateway log as `llm_billing_required`). The rest of this
> guide describes the Claude option.

This connects your robot to Claude so a child can ask a question out loud (Arabic or English)
and read the answer on the OLED. **No speaker is needed** — the robot runs in text-only mode until
you set `speaker on`. After setup the laptop can be switched off.

```
INMP441 mic ─▶ ESP32-S3 (detects end of speech) ─▶ HTTPS ─▶ AI gateway (online, always on)
                                                                ├─ Groq Whisper: speech → text (AR/EN)
                                                                ├─ safety checks + tutor (math is computed)
                                                                └─ Claude Haiku 5.5: the answer
OLED: listening bars → thinking dots → happy face → the answer scrolls → listening again
```

Your Anthropic API key and speech key live **only on the gateway**. The robot holds a device token
that works only with your gateway and can be revoked.

## What you need

| Account | Why | Notes |
|---|---|---|
| **Anthropic Console** (console.anthropic.com) | Claude API key | Separate from a Claude.ai subscription: the API is billed per token from prepaid credits. Add a small credit and set a **monthly spend limit**. |
| **Groq** (console.groq.com) | Speech-to-text (Whisper Large v3 Turbo, Arabic + English) | Create an API key. Alternative: OpenAI (`STT_PROVIDER=openai`, set `STT_MODEL`). |
| **Vercel** (vercel.com), Hobby plan | Hosts the gateway online, 24/7, free | [VERCEL_SETUP.md](VERCEL_SETUP.md). Alternatives: Render, Google Cloud Run, any container host with HTTPS (Dockerfile). |

## Costs (estimates — confirm on each provider's pricing page)

| Item | Price | Per question (estimate) |
|---|---|---|
| Claude Haiku 5.5 | $0.10 per million input tokens, $0.50 per million output tokens (prompts ≤ 100K tokens) — *official Anthropic pricing page* | ~1,200 input + up to ~400 output tokens (answer + thinking at low effort) ≈ **$0.0003** |
| Groq Whisper Large v3 Turbo | $0.04 per hour of audio, **minimum 10 s billed per request** — *third-party sources, Oct 2026* | ≥ 10 s billed ≈ **$0.0001** |
| Text-to-speech | not used in text-only mode | $0 |
| Gateway hosting | **Vercel Hobby: $0** (personal, non-commercial; free monthly allowance; Vercel pauses instead of charging when it runs out) — *Vercel docs*. Alternatives: Render Starter ≈ $7/month; Render's free tier sleeps and wakes too slowly for the robot — *third-party sources* | $0 on Vercel |

Example: 50 questions a day ≈ 1,500 a month ≈ **$0.45 Claude + $0.17 Groq**, plus hosting.
Each turn's real token counts are written to the gateway log (`in_tok`, `out_tok`), so you can
check the bill against actual use. Silence and noise are filtered on the gateway *before* the paid
speech service is called.

Model choice: Claude Haiku 5.5 is Anthropic's fast, low-cost model and fits short answers for a
child. To try a stronger model later, change one variable: `ANTHROPIC_MODEL=claude-sonnet-5-5`
($2 / $10 per million tokens) or `claude-opus-5-5` ($4 / $20).

## Step 1 — Create the robot's device token (on your laptop)

In PowerShell, in your project folder (`C:\Users\gdark\OneDrive\Documents\robot`):

```powershell
cd gateway
..\.venv\Scripts\python.exe -m robot_gateway.devices add robot-1 --file devices.json
```

It prints two things — keep both somewhere safe for a few minutes:

- `Token (shown ONCE ...): rbt_...` → goes **into the robot** (step 3)
- `DEVICE_TOKEN_HASHES=robot-1:...` → goes **into the gateway** (step 2)

`devices.json` is listed in `.gitignore`; never commit it.

## Step 2 — Put the gateway online

**Recommended: Vercel, free → follow [VERCEL_SETUP.md](VERCEL_SETUP.md)** (Parts B–D), then come
back to Step 3 here with your `https://….vercel.app` address.

Alternative — Render (paid Starter instance, runs the Docker image):

1. render.com → **New → Web Service** → connect GitHub → choose `gdarkstop-tech/ecu-repot-`.
2. Branch: `claude/vigilant-gauss-lvs43y` · **Root Directory: `gateway`** · Runtime: **Docker** ·
   Instance type: **Starter** (always on).
3. **Environment variables** (mark the keys as secret):

| Key | Value |
|---|---|
| `LLM_PROVIDER` | `anthropic` |
| `ANTHROPIC_API_KEY` | your Anthropic key |
| `ANTHROPIC_MODEL` | `claude-haiku-5-5` |
| `STT_PROVIDER` | `groq` |
| `GROQ_API_KEY` | your Groq key |
| `TTS_PROVIDER` | `mock` (no speaker yet) |
| `DEVICE_TOKEN_HASHES` | the `robot-1:...` line from step 1 |
| `ROBOT_NAME` | your robot's name |

4. Also add `CONTEXT_SECRET` (a long random text: see VERCEL_SETUP.md Part A, step 4).
   Health check path: `/healthz`. Deploy. When it is live, open
   `https://<your-service>.onrender.com/healthz` in a browser — you should see
   `"llm":"anthropic"`, `"stt":"groq"` and `"devices":1`.

5. Test it from the laptop exactly as the robot will call it (replace the URL and token):

```powershell
..\.venv\Scripts\python.exe ..\tools\gateway_client.py --url https://<your-service>.onrender.com --token rbt_... --text "What is seven plus five?"
```

A JSON answer with `"reply_text"` means Claude is connected. Try Arabic too:
`--text "لماذا السماء زرقاء؟"`.

## Step 3 — Provision the robot (USB, once)

Flash the robot firmware if you have not yet (README section 2), then:

```powershell
..\.venv\Scripts\python.exe ..\tools\robot_console.py provision
```

Enter: Wi-Fi name and password (**2.4 GHz** network — the ESP32-S3 has no 5 GHz radio), gateway URL
`https://<your-project>.vercel.app` (or your Render address), the `rbt_...` token, language (`auto`, `en` or `ar`), the
child's age, and answer **No** to "speaker installed". The robot restarts and joins Wi-Fi.

Language: `auto` lets the speech service detect Arabic or English for each question. If one
language is always used, setting `ar` or `en` gives better recognition of short phrases.

## Step 4 — First conversation (laptop still connected, to watch the log)

```powershell
..\.venv\Scripts\python.exe ..\tools\robot_console.py
```

Type `status` — check `"wifi":{"connected":true}` and `"gateway":{"configured":true}`. Type
`ask what is 7 plus 5` to test the network path without the microphone. Then press the **mute
button** (unmute) and speak: the OLED shows listening bars, thinking dots, a happy face, then the
answer scrolls. Events such as `{"evt":"turn","stage":"reply"}` appear in the console.

## Step 5 — Independent operation

Unplug the USB cable, power the robot from the power bank, press the button, ask a question. The
robot only needs Wi-Fi and the online gateway. If Wi-Fi drops, it shows offline learning cards
and reconnects by itself.

## If something goes wrong

| On the OLED / console | Meaning and fix |
|---|---|
| Wi-Fi-off icon stays | Wrong SSID/password or a 5 GHz-only network; re-run `provision` |
| `ERR auth_rejected` (stays) | Token and `DEVICE_TOKEN_HASHES` do not match — redo step 1 and update both |
| `ERR gateway_unavailable` / `gateway_timeout` | Gateway misconfigured or a provider is down — open `/healthz`; check the Vercel (or Render) logs |
| `ERR net_or_tls_error` | Wrong URL (must start with `https://`) or unusual certificate authority |
| Robot ignores you | Speak closer; check `mic_test`; quiet audio is ignored on purpose |
| Answer is about the wrong words | Set the language explicitly (`set lang ar` / `set lang en` in the console) |
| Gateway log shows `llm_auth_failed` | Anthropic key wrong or out of credit |
| Gateway log shows `stt_auth_failed` | Groq key wrong |
