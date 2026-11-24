"""The robot's personality: faces on command and reactions to rude words, compliments and laughter."""
import random

import pytest
from conftest import AUTH, make_client

from robot_gateway.pipeline import ALLOWED_EMOTIONS
from robot_gateway.reactions import FACE_LINES, MOODS, REACTION_MS, react
from test_llm_pipeline import FakeLLM


def r(text):
    return react(text, "en" if text.isascii() else "ar", random.Random(7))


# ------------------------------------------------------------------ faces on command
@pytest.mark.parametrize("text,emotion", [
    ("make a laughing face", "laughing"), ("show me your sad face", "sad"), ("do a scared face please", "scared"),
    ("can you make a dizzy face", "dizzy"), ("show me heart eyes", "love"), ("make star eyes", "starstruck"),
    ("be cool", "cool"), ("can you be shy", "shy"), ("act grumpy", "annoyed"), ("look surprised", "surprised"),
    ("laugh", "laughing"), ("can you cry", "crying"), ("please wink", "playful"), ("laughing face!", "laughing"),
    ("make a sneaky face", "mischievous"), ("make a bored face", "bored"), ("make a sleepy face", "sleepy"),
    # Arabic, Modern Standard and Egyptian
    ("اعمل وش زعلان", "sad"), ("اعمل وش ضاحك", "laughing"), ("وريني وشك الخايف", "scared"),
    ("اعمل عيون قلوب", "love"), ("وريني وش متفاجئ", "surprised"), ("اعمل وش مكسوف", "shy"),
    ("اعمل وش دايخ", "dizzy"), ("اعمل وش غضبان", "annoyed"), ("خليك روش", "cool"), ("كن سعيدا", None),
    ("كن سعيد", "happy"), ("اضحك", "laughing"), ("عيط", "crying"), ("اغمز", "playful"), ("يلا اضحك", "laughing"),
])
def test_face_on_command(text, emotion):
    got = r(text)
    if emotion is None:  # "سعيدا" (accusative) is not in the list: nothing is guessed
        assert got is None or got.kind != "command"
        return
    assert got is not None and got.kind == "command" and got.emotion == emotion, text


def test_unknown_face_lists_what_the_robot_can_do():
    for text in ("make a face", "show me your faces", "اعمل وش"):
        got = r(text)
        assert got.kind == "faces" and got.emotion == "excited"


@pytest.mark.parametrize("text", [
    "tell me a story", "what is 7 plus 5", "look at the stars", "I feel sad", "I get scared at night",
    "make me laugh", "don't cry", "dont cry", "I cry a lot", "hi", "the ugly duckling story", "is fire good or bad",
    "teach me the alphabet", "why is the sky blue", "علمني الحروف", "احكيلي قصة عن وحش", "احكيلي عن حيوان",
    "وش اسمك", "يا صاحبي احكيلي قصة", "حيوان", "كلب", "كم ستة ضرب سبعة",
])
def test_ordinary_requests_are_not_reactions(text):
    assert r(text) is None, text


# ------------------------------------------------------------------ rude words, compliments, moments
@pytest.mark.parametrize("text,kind,emotion", [
    ("you are stupid", "insult", "annoyed"), ("you're so dumb", "insult", "annoyed"), ("idiot", "insult", "annoyed"),
    ("you are ugly", "insult", "annoyed"), ("shut up", "insult", "annoyed"), ("I hate you", "insult", "crying"),
    ("يا غبي", "insult", "annoyed"), ("انت حمار", "insult", "annoyed"), ("اسكت", "insult", "annoyed"),
    ("اخرس", "insult", "annoyed"), ("بكرهك", "insult", "crying"), ("انت وحش", "insult", "annoyed"),
    ("I am so stupid", "self_talk", "love"), ("i'm dumb", "self_talk", "love"), ("انا غبي", "self_talk", "love"),
    ("انا فاشل", "self_talk", "love"),
    ("I love you", "compliment", "love"), ("you are my best friend", "compliment", "love"),
    ("you are so cute", "compliment", "shy"), ("you are awesome", "compliment", "cool"),
    ("good robot", "compliment", "proud"), ("you're funny", "compliment", "laughing"),
    ("بحبك", "compliment", "love"), ("انت حلو اوي", "compliment", "shy"), ("انت شاطر", "compliment", "proud"),
    ("انت روش", "compliment", "cool"), ("دمك خفيف", "compliment", "laughing"),
    ("ha ha ha", "laugh", "laughing"), ("hahaha", "laugh", "laughing"), ("هههههه", "laugh", "laughing"),
    ("tickle tickle", "tickle", "laughing"), ("زغزغ", "tickle", "laughing"),
    ("boo", "boo", "scared"), ("بخ", "boo", "scared"),
    ("this is boring", "bored", "bored"), ("I'm bored", "bored", "bored"), ("زهقت", "bored", "bored"),
])
def test_reactions(text, kind, emotion):
    got = r(text)
    assert got is not None and (got.kind, got.emotion) == (kind, emotion), text
    assert got.text and got.oled


def test_every_face_and_line_is_one_the_robot_can_show():
    assert set(MOODS) == set(FACE_LINES)
    assert set(FACE_LINES) <= ALLOWED_EMOTIONS
    for en, ar, oled_en, oled_ar in FACE_LINES.values():
        assert len(en) <= 60 and len(ar) <= 60 and len(oled_en) <= 20 and len(oled_ar) <= 20  # short to say
    system_faces = {"speaking", "listening", "muted", "offline", "error", "sleeping", "boot", "blink"}
    assert not (ALLOWED_EMOTIONS & system_faces)


def test_replies_follow_the_language():
    assert r("make a laughing face").text.startswith("Ha ha")
    assert "ها ها" in r("اعمل وش ضاحك").text
    assert react("make a laughing face", "ar").text == FACE_LINES["laughing"][1]


# ------------------------------------------------------------------ through the gateway
def test_reaction_reply_has_the_face_and_how_long_to_hold_it_and_never_calls_the_model():
    llm = FakeLLM("should not be used")
    c = make_client(llm=llm)
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "make a laughing face"}).json()
    assert b["emotion"] == "laughing" and b["emotion_ms"] == REACTION_MS and b["source"] == "reaction"
    assert b["reply_text"].startswith("Ha ha") and b["display"]["oled_text"]
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "يا غبي"}).json()
    assert b["emotion"] == "annoyed" and b["lang"] == "ar" and b["display"]["rtl"]
    assert llm.calls == []  # deterministic: no model, no quota used
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "why do cats purr?"}).json()
    assert "emotion_ms" not in b and len(llm.calls) == 1  # ordinary answers keep the robot's default


def test_safety_comes_before_reactions():
    c = make_client()
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "nobody loves me, I love you"}).json()
    assert b["source"] == "safety" and b["emotion"] == "worried"


def test_a_reaction_during_practice_keeps_the_practice_item():
    c = make_client()
    c.post("/v1/turns/text", headers=AUTH, json={"text": "teach me the alphabet"})
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "you are stupid"}).json()
    assert b["emotion"] == "annoyed"
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": "apple"}).json()
    assert b["display"]["content"]["value"] == "B"  # the drill was still waiting for A's word


def test_a_practice_answer_is_never_taken_as_a_reaction():
    c = make_client()
    c.post("/v1/turns/text", headers=AUTH, json={"text": "teach me english words"})
    # Whatever the first word is, saying it is a practice answer, not a face command or a compliment.
    first = c.post("/v1/turns/text", headers=AUTH, json={"text": "teach me english words"}).json()
    word = first["display"]["content"]["value"]
    b = c.post("/v1/turns/text", headers=AUTH, json={"text": word.lower()}).json()
    assert b["source"] != "reaction"
