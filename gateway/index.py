"""Vercel entrypoint (zero-configuration FastAPI): Vercel imports this file and serves `app`.

Set the project's Root Directory to `gateway` and the environment variables listed in
docs/VERCEL_SETUP.md. Serverless instances keep nothing between requests, so the gateway
runs stateless here: conversation memory travels with the robot as a sealed token, and API
keys are read from Vercel's environment variables (never from the repository or the robot).
Local runs and Docker use robot_gateway.app:app_factory instead.
"""
import logging

from robot_gateway.app import create_app

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")

app = create_app(stateless=True)
