import shutil

import pytest

pytestmark = pytest.mark.skipif(not shutil.which("espeak-ng"), reason="espeak-ng not installed")


def test_espeak_speaks_english_and_arabic():
    from robot_gateway.providers.espeak import EspeakTTS

    tts = EspeakTTS()
    for text, lang in [("Hello friend", "en"), ("مرحبا يا صديقي", "ar"), ("--help", "en")]:
        sp = tts.synthesize(text, lang)
        assert sp.sample_rate in (16000, 22050, 24000)
        assert len(sp.pcm) > sp.sample_rate * 2 * 0.3  # more than 0.3 s of audio
