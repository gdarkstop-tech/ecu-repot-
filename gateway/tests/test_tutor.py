import random

from robot_gateway.tutor.engine import Session, Tutor


def fresh(seed=1):
    s = Session("d")
    s.rng = random.Random(seed)
    return s


def test_alphabet_progresses_in_english_and_arabic():
    t, s = Tutor(), fresh()
    r = t.handle(s, "teach me the alphabet", "en", 6)
    assert r.content.value == "A" and "Apple" in r.text
    r = t.handle(s, "apple", "en", 6)
    assert r.content.value == "B"
    s2 = fresh()
    r = t.handle(s2, "علمني الحروف", "ar", 6)
    assert r.content.value == "ا" and "أسد" in r.text


def test_counting_checks_next_number():
    t, s = Tutor(), fresh()
    r = t.handle(s, "let's count", "en", 5)
    assert r.text.startswith("Let's count together! 1, 2, 3")
    r = t.handle(s, "four!", "en", 5)
    assert r.emotion == "happy"


def test_math_answer_feedback_two_attempts_then_reveal():
    t, s = Tutor(), fresh(3)
    t.handle(s, "math", "en", 6)
    expected = s.pending.expected_number
    wrong = str(int(expected) + 1)
    r = t.handle(s, wrong, "en", 6)
    assert "Try again" in r.text and s.pending is not None
    r = t.handle(s, wrong, "en", 6)
    assert f"The answer is {int(expected)}" in r.text and r.emotion == "sad"
    exp2 = s.pending.expected_number  # next question asked
    r = t.handle(s, f"it is {int(exp2)}", "en", 6)
    assert r.emotion == "happy" and "Correct" in r.text or "right" in r.text or "smart" in r.text


def test_difficulty_follows_age_and_streak():
    t = Tutor()
    young = fresh(5)
    t.handle(young, "math", "en", 4)
    for _ in range(20):
        a, op, b, ans = t.make_problem(young)
        assert op == "+" and a <= 5 and b <= 5
    older = fresh(5)
    t.handle(older, "math", "en", 9)
    assert older.level == 2
    for _ in range(3):
        t.handle(older, str(int(older.pending.expected_number)), "en", 9)
    assert older.level == 3  # three right in a row: harder


def test_math_problems_never_negative():
    t = Tutor()
    s = fresh(11)
    s.level = 2
    for _ in range(500):
        assert t.make_problem(s)[3] >= 0


def test_quiz_scores_five_questions():
    t, s = Tutor(), fresh(7)
    r = t.handle(s, "quiz", "en", 7)
    for _ in range(5):
        r = t.handle(s, "I don't know", "en", 7)  # a child who doesn't know must still progress
        if "Quiz finished" in r.text:
            break
    assert "Quiz finished" in r.text and "0 out of 5" in r.text


def test_non_number_reply_nudges_once_then_counts():
    t, s = Tutor(), fresh(3)
    t.handle(s, "math", "en", 6)
    assert "Tell me a number" in t.handle(s, "banana", "en", 6).text
    assert "Try again" in t.handle(s, "banana", "en", 6).text


def test_vision_is_honest():
    r = Tutor().handle(fresh(), "what do you see?", "en", 6)
    assert "camera is not connected" in r.text


def test_story_and_wisdom_are_curated():
    t, s = Tutor(), fresh()
    r = t.handle(s, "tell me a story", "en", 6)
    assert "turtle" in r.text
    r = t.handle(s, "another story please", "en", 6)
    assert "Lina" in r.text
    r = t.handle(fresh(), "حكمة", "ar", 6)
    assert r.lang == "ar"


def test_free_question_not_handled_by_tutor():
    assert Tutor().handle(fresh(), "why do cats purr?", "en", 6) is None
