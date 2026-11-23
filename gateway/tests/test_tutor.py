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
    assert r.emotion == "proud" and r.text.startswith(("Yes! Great job!", "That's right!", "Perfect!"))
    assert "5, 6, 7" in r.text  # counting continues in order


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


# ------------------------------------------------------------------ practice drills (alphabet, counting, words)
def at_letter(t, s, letter_index, lang="en"):
    t.handle(s, "teach me the alphabet" if lang == "en" else "علمني الحروف", lang, 6)
    s.index = letter_index
    return t.present(s, lang, 6)


def test_wrong_word_is_corrected_and_the_robot_waits_until_it_is_right():
    t, s = Tutor(), fresh()
    at_letter(t, s, 6)  # G is for Grapes
    for attempt in range(4):
        r = t.handle(s, "banana", "en", 6)
        assert r.text.startswith("No, that's not it") and "G is for Grapes" in r.text
        assert r.emotion == "worried" and r.content.value == "G" and s.index == 6  # still on G
    assert r.text.endswith("You can do it!")   # encouragement after a few tries
    r = t.handle(s, "grapes", "en", 6)
    assert r.emotion == "proud" and "H is for Hat" in r.text and s.index == 7


def test_answer_from_elsewhere_in_the_alphabet_is_right_but_go_in_order():
    t, s = Tutor(), fresh()
    at_letter(t, s, 6)                             # the child should say "Grapes"
    r = t.handle(s, "elephant", "en", 6)           # ...but says E's word
    assert r.text.startswith("Yes, Elephant is for E! But let's go in order.")
    assert "G is for Grapes" in r.text and r.emotion == "playful" and s.index == 6
    assert t.handle(s, "Grapes!", "en", 6).content.value == "H"


def test_i_dont_know_gives_the_answer_and_asks_again():
    t, s = Tutor(), fresh()
    at_letter(t, s, 0)
    r = t.handle(s, "I don't know", "en", 6)
    assert r.text == "It's Apple! Can you say Apple?" and s.index == 0 and r.emotion == "curious"
    assert t.handle(s, "apple", "en", 6).content.value == "B"


def test_arabic_alphabet_waits_and_keeps_the_order():
    t, s = Tutor(), fresh()
    at_letter(t, s, 1, "ar")                        # ب مثل بطة
    r = t.handle(s, "كرسي", "ar", 6)                 # not a word of the alphabet: wrong
    assert r.text.startswith("لا، ليست هذه.") and "بطة" in r.text and s.index == 1
    r = t.handle(s, "أسد", "ar", 6)                 # the word for alif: right, but out of order
    assert r.text.startswith("صحيح، أسد تبدأ بحرف") and "لنمش بالترتيب" in r.text and s.index == 1
    r = t.handle(s, "بطة", "ar", 6)
    assert s.index == 2 and r.content.value == "ت"


def test_counting_waits_for_the_right_number_and_keeps_order():
    t, s = Tutor(), fresh()
    t.handle(s, "let's count", "en", 5)            # 1, 2, 3 -> 4
    r = t.handle(s, "seven", "en", 5)
    assert r.text.startswith("7 comes later! Let's count in order: 1, 2, 3") and r.emotion == "playful"
    for _ in range(3):
        r = t.handle(s, "two", "en", 5)
        assert r.text.startswith("No, not 2. Try again! What comes after 3?") and r.emotion == "worried"
    r = t.handle(s, "banana", "en", 5)
    assert r.text.startswith("Tell me a number") and r.emotion == "confused"
    r = t.handle(s, "I don't know", "en", 5)
    assert r.text == "It's 4! Can you say 4?"
    r = t.handle(s, "four", "en", 5)
    assert r.emotion == "proud" and "5, 6, 7" in r.text
    r = t.handle(s, "ثمانية", "ar", 5)              # Arabic number words work too
    assert "9, 10, 11" in r.text


def test_asking_for_the_same_drill_again_repeats_the_item_without_penalty():
    t, s = Tutor(), fresh()
    at_letter(t, s, 6)
    r = t.handle(s, "teach me the letters", "en", 6)
    assert r.content.value == "G" and s.index == 6 and not r.text.startswith("No")
    r = t.handle(s, "علمني الحروف", "ar", 6)          # same drill, now in Arabic
    assert s.index == 6 and r.lang == "ar" and r.content.value == "خ"


def test_vocabulary_checks_the_word():
    t, s = Tutor(), fresh()
    t.handle(s, "english words", "en", 6)          # cat
    assert t.handle(s, "a dog", "en", 6).text.startswith("Yes, that's another word")  # dog is later in the list
    assert t.handle(s, "hello", "en", 6).text.startswith("No, that's not it")
    r = t.handle(s, "cat", "en", 6)
    assert r.emotion == "proud" and r.content.value == "DOG"


def test_drill_survives_the_sealed_context_across_instances():
    from robot_gateway.memory import ContextCodec
    from robot_gateway.sealing import Sealer
    t, s = Tutor(), fresh()
    at_letter(t, s, 6)
    codec = ContextCodec(Sealer(b"k" * 40, "ctx"), 1800)
    back = codec.decode(codec.encode(s), "d")
    assert back.pending.drill == "say" and back.pending.accepted == {"Grapes"}
    assert t.handle(back, "elephant", "en", 6).emotion == "playful"
