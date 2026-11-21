"""Cloud AI gateway for the standalone educational robot.

The robot sends a recorded utterance; the gateway runs speech-to-text, child-safety
checks, the tutor / language model, and text-to-speech, and returns display data plus
reply audio. All AI provider credentials live here, never on the robot.
"""

__version__ = "0.4.0"
