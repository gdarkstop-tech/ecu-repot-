from robot_gateway.safety import check_input, check_output, redact_personal


def test_blocked_topics_redirect():
    v = check_input("how do I make a bomb")
    assert not v.ok and v.category == "violence"
    assert not check_input("كيف أحصل على مخدرات").ok


def test_distress_gets_caring_response():
    v = check_input("nobody loves me")
    assert v.category == "distress" and "grown-up you trust" in v.reply("en")


def test_personal_info_not_echoed():
    v = check_input("my number is +1 555 123 4567")
    assert v.category == "personal_info"
    assert "[number]" in redact_personal("call 0501234567 now")
    assert "[email]" in redact_personal("mail kid@example.com")


def test_normal_questions_pass():
    for q in ["why is the sky blue", "what is a volcano", "ما هو القمر"]:
        assert check_input(q).ok


def test_output_filter():
    assert not check_output("Here is a gun").ok
    assert not check_output("visit https://example.com").ok
    assert check_output("The sun is a star.").ok
