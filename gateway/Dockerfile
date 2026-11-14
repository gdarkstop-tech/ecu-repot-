# Robot AI gateway — deploy to any container host with HTTPS (Cloud Run, Fly.io, Render,
# Railway, a VPS behind Caddy/nginx...). The robot talks to it directly over the internet,
# so your laptop does not need to be running.
FROM python:3.12-slim

# eSpeak NG: free offline English/Arabic voice used when no paid TTS is configured.
RUN apt-get update \
    && apt-get install -y --no-install-recommends espeak-ng \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY robot_gateway ./robot_gateway

# Run as an unprivileged user.
RUN useradd --create-home --uid 10001 gateway
USER gateway

ENV PORT=8080 PYTHONUNBUFFERED=1
EXPOSE 8080
# One worker: sessions, rate limits and reply audio are kept in memory (see docs/ARCHITECTURE.md).
CMD ["sh", "-c", "uvicorn --factory robot_gateway.app:app_factory --host 0.0.0.0 --port ${PORT} --workers 1 --proxy-headers"]
